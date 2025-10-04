/**
 * Tests that the TTL deleter accesses the storage engine with low priority.
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
            ttlMonitorSleepSecs: 1,
            ttlMonitorEnabled: false,
            storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactionsWithPrioritization",
        },
    },
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = jsTestName();
const coll = primary.getDB(dbName).coll;

// Create a TTL index to expire documents after 0 seconds.
assert.commandWorked(coll.createIndex({expireAt: 1}, {expireAfterSeconds: 0}));

// Insert enough documents to ensure the cleanup work is substantial.
const bulk = coll.initializeUnorderedBulkOp();
const numDocs = 2000;
const pastDate = new Date(Date.now() - 5000); // A time in the recent past.

for (let i = 0; i < numDocs; i++) {
    // Add a payload to make the documents non-trivial in size.
    bulk.insert({_id: i, expireAt: pastDate, payload: "x".repeat(1024)});
}
assert.commandWorked(bulk.execute());
assert.eq(numDocs, coll.countDocuments({}));

/**
 * Helper function to get the number of finished low-priority write operations on the primary.
 */
const numLowPriorityWrites = function () {
    const status = primary.adminCommand({serverStatus: 1});
    return status.queues.execution.low.write.normalPriority.finishedProcessing;
};

const lowPriorityBefore = numLowPriorityWrites();

assert.commandWorked(primary.adminCommand({setParameter: 1, ttlMonitorEnabled: true}));

// Wait for the TTL monitor to run and delete all the expired documents.
assert.soon(
    () => coll.countDocuments({}) === 0,
    "TTL monitor did not delete all documents within the timeout",
    10 * 1000,
);

const lowPriorityAfter = numLowPriorityWrites();

// Measure the counter again and assert that it has increased.
assert.gt(
    lowPriorityAfter,
    lowPriorityBefore,
    "The number of low-priority writes should increase after a TTL deletion runs",
);

rst.stopSet();
