/**
 * Tests that the background range deletion task on a donor shard after a chunk migration completes
 * accesses the storage engine with low priority.
 *
 * @tags: [
 *    requires_wiredtiger,
 *    requires_sharding,
 *    featureFlagMultipleTicketPoolsExecutionControl,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    other: {
        rsOptions: {
            setParameter: {
                storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactionsWithPrioritization",
            },
        },
    },
});

const dbName = jsTestName();
const coll = st.s.getDB(dbName).coll;
const ns = coll.getFullName();

// Set up a sharded collection.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

// Insert enough documents to ensure the cleanup work is substantial.
const bulk = coll.initializeUnorderedBulkOp();
const numDocs = 2000;
for (let i = 0; i < numDocs; i++) {
    // Add a payload to make the documents non-trivial in size.
    bulk.insert({_id: i, payload: "x".repeat(1024)});
}
assert.commandWorked(bulk.execute());

// Split the collection into two chunks to prepare for migration.
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: numDocs / 2}}));

const donor = st.shard0;
const recipient = st.shard1;

/**
 * Helper function to get the number of finished low-priority write operations on the donor.
 */
const numLowPriorityWrites = function () {
    const status = donor.adminCommand({serverStatus: 1});
    return status.queues.execution.write.normalPriority.finishedProcessing;
};

const lowPriorityBefore = numLowPriorityWrites();

// Move a chunk from the donor (shard0) to the recipient (shard1) while waiting for the range
// deletion to complete.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: recipient.shardName, _waitForDelete: true}));

const lowPriorityAfter = numLowPriorityWrites();

// Measure the counter again and assert that it has increased.
assert.gt(
    lowPriorityAfter,
    lowPriorityBefore,
    "The number of low-priority writes should increase after a range deletion runs",
);

st.stop();
