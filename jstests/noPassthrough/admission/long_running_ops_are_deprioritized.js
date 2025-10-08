/**
 * Tests that long-running operations access the storage engine with low priority.
 *
 * @tags: [
 *      requires_wiredtiger,
 *      featureFlagMultipleTicketPoolsExecutionControl,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Force the query to yield frequently to better expose the low-priority behavior.
            internalQueryExecYieldIterations: 1,
            storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactionsWithPrioritization",
        },
    },
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB(jsTestName());
const coll = db.coll;

/**
 * Generates a random string of a given length.
 */
function generateRandomString(length) {
    const chars = "abcdefghijklmnopqrstuvwxyz";
    let result = "";
    for (let i = 0; i < length; i++) {
        result += chars.charAt(Math.floor(Math.random() * chars.length));
    }
    return result;
}

/**
 * Helper function to get the number of finished low-priority read operations on the primary.
 */
const numLowPriorityReads = function () {
    const status = primary.adminCommand({serverStatus: 1});
    return status.queues.execution.read.lowPriority.finishedProcessing;
};

assert.commandWorked(db.createCollection(coll.getName()));

// Insert a substantial number of documents to make the scan more expensive.
const bulk = coll.initializeUnorderedBulkOp();
const numDocs = 5000;
for (let i = 0; i < numDocs; i++) {
    // Add a payload to make the documents non-trivial in size, and a random string for the regex.
    bulk.insert({_id: i, payload: "x".repeat(512), randomStr: generateRandomString(100)});
}
assert.commandWorked(bulk.execute());

// Add indexes to test index scan behavior.
assert.commandWorked(coll.createIndex({payload: 1}));
assert.commandWorked(coll.createIndex({randomStr: 1}));

const testUnboundedCollectionScanIsDeprioritized = function (direction) {
    const lowPriorityBefore = numLowPriorityReads();
    const explain = coll.find().hint({$natural: direction}).explain();
    assert.eq("COLLSCAN", explain.queryPlanner.winningPlan.stage, tojson(explain));

    assert.eq(numDocs, coll.find().hint({$natural: direction}).itcount());

    assert.gt(numLowPriorityReads(), lowPriorityBefore, `Scan with direction ${direction} should be deprioritized`);
};
testUnboundedCollectionScanIsDeprioritized(1);
testUnboundedCollectionScanIsDeprioritized(-1);

const testUnboundedIndexScanIsDeprioritized = function () {
    const lowPriorityBefore = numLowPriorityReads();
    const query = {payload: {$gte: ""}};

    const explain = coll.find(query).sort({payload: 1}).explain();
    assert.eq("FETCH", explain.queryPlanner.winningPlan.stage);
    assert.eq("IXSCAN", explain.queryPlanner.winningPlan.inputStage.stage);

    assert.eq(numDocs, coll.find(query).sort({payload: 1}).itcount());

    assert.gt(numLowPriorityReads(), lowPriorityBefore, "Unbounded index scan should be deprioritized");
};
testUnboundedIndexScanIsDeprioritized();

const testLongRunningRegexIsDeprioritized = function () {
    const lowPriorityBefore = numLowPriorityReads();

    const query = {randomStr: {$regex: "a.*b.*c.*d"}};

    const explain = coll.find(query).hint({$natural: 1}).explain();
    assert.eq("COLLSCAN", explain.queryPlanner.winningPlan.stage);

    coll.find(query).hint({$natural: 1}).itcount();

    assert.gt(numLowPriorityReads(), lowPriorityBefore, "Long-running regex query should be deprioritized");
};
testLongRunningRegexIsDeprioritized();

const testIndexedLongRunningRegexIsDeprioritized = function () {
    const lowPriorityBefore = numLowPriorityReads();

    const query = {randomStr: {$regex: "a.*b.*c.*d"}};

    const explain = coll.find(query).explain();
    assert.eq("FETCH", explain.queryPlanner.winningPlan.stage);
    assert.eq("IXSCAN", explain.queryPlanner.winningPlan.inputStage.stage);

    coll.find(query).itcount();

    assert.gt(numLowPriorityReads(), lowPriorityBefore, "Indexed long-running regex query should be deprioritized");
};
testIndexedLongRunningRegexIsDeprioritized();

rst.stopSet();
