/**
 * Tests that long-running operations access the storage engine with low priority.
 *
 * @tags: [
 *      requires_wiredtiger,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {getWinningPlanFromExplain, isCollscan, isIxscan} from "jstests/libs/query/analyze_plan.js";

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

/**
 * Helper function to get the number of finished low-priority write operations on the primary.
 */
const numLowPriorityWrites = function () {
    const status = primary.adminCommand({serverStatus: 1});
    return status.queues.execution.write.lowPriority.finishedProcessing;
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
    assert(isCollscan(db, getWinningPlanFromExplain(explain)));

    assert.eq(numDocs, coll.find().hint({$natural: direction}).itcount());

    assert.gt(numLowPriorityReads(), lowPriorityBefore, `Scan with direction ${direction} should be deprioritized`);
};
testUnboundedCollectionScanIsDeprioritized(1);
testUnboundedCollectionScanIsDeprioritized(-1);

const testUnboundedIndexScanIsDeprioritized = function () {
    const lowPriorityBefore = numLowPriorityReads();
    const query = {payload: {$gte: ""}};

    const explain = coll.find(query).sort({payload: 1}).explain();
    assert(isIxscan(db, getWinningPlanFromExplain(explain)));

    assert.eq(numDocs, coll.find(query).sort({payload: 1}).itcount());

    assert.gt(numLowPriorityReads(), lowPriorityBefore, "Unbounded index scan should be deprioritized");
};
testUnboundedIndexScanIsDeprioritized();

const testLongRunningRegexIsDeprioritized = function () {
    const lowPriorityBefore = numLowPriorityReads();

    const query = {randomStr: {$regex: "a.*b.*c.*d"}};

    const explain = coll.find(query).hint({$natural: 1}).explain();
    assert(isCollscan(db, getWinningPlanFromExplain(explain)));

    coll.find(query).hint({$natural: 1}).itcount();

    assert.gt(numLowPriorityReads(), lowPriorityBefore, "Long-running regex query should be deprioritized");
};
testLongRunningRegexIsDeprioritized();

const testIndexedLongRunningRegexIsDeprioritized = function () {
    const lowPriorityBefore = numLowPriorityReads();

    const query = {randomStr: {$regex: "a.*b.*c.*d"}};

    const explain = coll.find(query).explain();
    assert(isIxscan(db, getWinningPlanFromExplain(explain)));

    coll.find(query).itcount();

    assert.gt(numLowPriorityReads(), lowPriorityBefore, "Indexed long-running regex query should be deprioritized");
};
testIndexedLongRunningRegexIsDeprioritized();

const testMultiDocumentTransactionIsNotDeprioritized = function () {
    // Even though this transaction contains an operation that would normally be deprioritized (an
    // unbounded collection scan), the transaction itself should run with normal priority.
    const lowPriorityBefore = numLowPriorityReads();

    const session = db.getMongo().startSession();
    const sessionColl = session.getDatabase(db.getName()).getCollection(coll.getName());

    session.startTransaction();
    // Perform an unbounded collection scan within the transaction.
    assert.eq(numDocs, sessionColl.find().hint({$natural: 1}).itcount());
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    assert.eq(numLowPriorityReads(), lowPriorityBefore, "Multi-document transaction should not be deprioritized");
};
testMultiDocumentTransactionIsNotDeprioritized();

const testMultiInsertsAreDeprioritized = function () {
    const lowPriorityBefore = numLowPriorityWrites();

    // Create a large batch of documents to insert.
    const batchSize = 1000;
    const docs = [];
    for (let i = numDocs; i < numDocs + batchSize; i++) {
        docs.push({_id: i, payload: "x".repeat(512), randomStr: generateRandomString(100)});
    }

    // Perform multi-inserts which should be deprioritized due to their size.
    assert.commandWorked(coll.insertMany(docs));

    assert.gt(numLowPriorityWrites(), lowPriorityBefore, "Multi inserts should be deprioritized");
};
testMultiInsertsAreDeprioritized();

const testMultiUpdatesAreDeprioritized = function () {
    const lowPriorityBefore = numLowPriorityWrites();

    // Perform a multi update that affects many documents.
    const result = coll.updateMany({payload: {$exists: true}}, {$set: {updated: true, updateTime: new Date()}});
    assert.gt(result.modifiedCount, 100);

    assert.gt(numLowPriorityWrites(), lowPriorityBefore, "Multi updates should be deprioritized");
};
testMultiUpdatesAreDeprioritized();

const testBulkWritesAreDeprioritized = function () {
    const lowPriorityBefore = numLowPriorityWrites();

    // Create a bulk write operation with mixed operations.
    const bulk = coll.initializeUnorderedBulkOp();

    // Add some inserts.
    const startId = numDocs + 1000;
    for (let i = startId; i < startId + 200; i++) {
        bulk.insert({_id: i, bulkInsert: true, payload: "y".repeat(256)});
    }

    // Add some updates.
    bulk.find({payload: {$regex: /^x/}}).update({$set: {bulkUpdated: true}});

    // Add some deletes.
    bulk.find({_id: {$gte: numDocs - 50, $lt: numDocs}}).remove();

    // Execute the bulk write.
    assert.commandWorked(bulk.execute());

    assert.gt(numLowPriorityWrites(), lowPriorityBefore, "Bulk writes should be deprioritized");
};
testBulkWritesAreDeprioritized();

const testMultiDeletesAreDeprioritized = function () {
    const lowPriorityBefore = numLowPriorityWrites();

    // Insert some documents to delete.
    const docsToDelete = [];
    for (let i = 0; i < 100; i++) {
        docsToDelete.push({_id: `delete_${i}`, toDelete: true, payload: "z".repeat(128)});
    }
    assert.commandWorked(coll.insertMany(docsToDelete));

    const deleteResult = coll.deleteMany({toDelete: true});
    assert.gt(deleteResult.deletedCount, 50);

    assert.gt(numLowPriorityWrites(), lowPriorityBefore, "Multi deletes should be deprioritized");
};
testMultiDeletesAreDeprioritized();

rst.stopSet();
