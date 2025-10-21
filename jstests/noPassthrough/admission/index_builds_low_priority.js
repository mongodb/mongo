/**
 * Tests that index builds access the storage engine with low priority on both primary and secondary
 * nodes.
 *
 * @tags: [
 *      requires_wiredtiger,
 *      requires_replication,
 *      featureFlagMultipleTicketPoolsExecutionControl,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactionsWithPrioritization",
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

const primaryLowPriorityBefore = numLowPriorityWrites(primary);
const secondaryLowPriorityBefore = numLowPriorityWrites(secondary);

// Build the index. This command blocks until the index is ready on the primary.
assert.commandWorked(coll.createIndex({x: 1}));

// Wait for the index build to complete on the secondary.
IndexBuildTest.waitForIndexBuildToStop(secondaryDB);

const primaryLowPriorityAfter = numLowPriorityWrites(primary);
const secondaryLowPriorityAfter = numLowPriorityWrites(secondary);

// Measure the counters again and assert that they have increased.
assert.gt(
    primaryLowPriorityAfter,
    primaryLowPriorityBefore,
    "The number of low-priority writes should increase on the primary after an index build",
);

assert.gt(
    secondaryLowPriorityAfter,
    secondaryLowPriorityBefore,
    "The number of low-priority writes should increase on the secondary after an index build",
);

rst.stopSet();
