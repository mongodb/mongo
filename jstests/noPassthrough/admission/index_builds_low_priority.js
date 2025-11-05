/**
 * Tests that index builds access the storage engine with low priority on both primary and secondary
 * nodes when enabled, and with normal priority when storageEngineDeprioritizeBackgroundTasks is
 * disabled.
 *
 * @tags: [
 *      requires_wiredtiger,
 *      requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactionsWithPrioritization",
            storageEngineHeuristicDeprioritizationEnabled: false,
        },
    },
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const dbName = jsTestName();
const coll = primary.getDB(dbName).coll;
const secondaryDB = secondary.getDB(dbName);

// Insert enough documents to ensure the index build is substantial.
const bulk = coll.initializeUnorderedBulkOp();
const numDocs = 2000;
for (let i = 0; i < numDocs; i++) {
    // Add a payload to make the documents non-trivial in size.
    bulk.insert({_id: i, x: `value_${i}`, payload: "x".repeat(1024)});
}
assert.commandWorked(bulk.execute());

/**
 * Helper function to get the number of finished low-priority write operations on a given node.
 */
const numLowPriorityWrites = function (node) {
    const status = node.adminCommand({serverStatus: 1});
    return status.queues.execution.write.lowPriority.finishedProcessing;
};

/**
 * Runs an index build and checks the low-priority write counters on both primary and secondary.
 */
const runIndexBuildTest = function ({coll, secondaryDB, indexSpec, expectLowPriorityWrites}) {
    const primaryLowPriorityBefore = numLowPriorityWrites(primary);
    const secondaryLowPriorityBefore = numLowPriorityWrites(secondary);

    // Build the index. This command blocks until the index is ready on the primary.
    assert.commandWorked(coll.createIndex(indexSpec));

    // Wait for the index build to complete on the secondary.
    IndexBuildTest.waitForIndexBuildToStop(secondaryDB);

    const primaryLowPriorityAfter = numLowPriorityWrites(primary);
    const secondaryLowPriorityAfter = numLowPriorityWrites(secondary);

    const assertFn = expectLowPriorityWrites ? assert.gt : assert.eq;
    assertFn(primaryLowPriorityAfter, primaryLowPriorityBefore);
    assertFn(secondaryLowPriorityAfter, secondaryLowPriorityBefore);
};

runIndexBuildTest({coll: coll, secondaryDB: secondaryDB, indexSpec: {x: 1}, expectLowPriorityWrites: true});

jsTest.log.info("Disabling storageEngineDeprioritizeBackgroundTasks on primary and secondary...");
assert.commandWorked(primary.adminCommand({setParameter: 1, storageEngineDeprioritizeBackgroundTasks: false}));
assert.commandWorked(secondary.adminCommand({setParameter: 1, storageEngineDeprioritizeBackgroundTasks: false}));

runIndexBuildTest({coll: coll, secondaryDB: secondaryDB, indexSpec: {y: 1}, expectLowPriorityWrites: false});

rst.stopSet();
