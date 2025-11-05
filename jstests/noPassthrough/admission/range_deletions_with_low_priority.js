/**
 * Tests that the background range deletion task on a donor shard after a chunk migration completes
 * accesses the storage engine with low priority when enabled, and with normal priority when
 * executionControlDeprioritizeBackgroundTasks is disabled.
 *
 * @tags: [
 *    requires_wiredtiger,
 *    requires_sharding,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    other: {
        rsOptions: {
            setParameter: {
                executionControlConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactionsWithPrioritization",
                executionControlHeuristicDeprioritizationEnabled: false,
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
let bulk = coll.initializeUnorderedBulkOp();
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
const numLowPriorityWrites = function (node) {
    const status = node.adminCommand({serverStatus: 1});
    return status.queues.execution.write.lowPriority.finishedProcessing;
};

let lowPriorityBefore = numLowPriorityWrites(donor);

// Move a chunk from the donor (shard0) to the recipient (shard1) while waiting for the range
// deletion to complete.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: recipient.shardName, _waitForDelete: true}));

let lowPriorityAfter = numLowPriorityWrites(donor);

// Measure the counter again and assert that it has increased.
assert.gt(
    lowPriorityAfter,
    lowPriorityBefore,
    "The number of low-priority writes should increase after a range deletion runs",
);

// Disable executionControlDeprioritizeBackgroundTasks so that range deletions run with normal
// priority. Return the chunk back to the donor and verify no low-priority writes are done there.
assert.commandWorked(recipient.adminCommand({setParameter: 1, executionControlDeprioritizeBackgroundTasks: false}));

lowPriorityBefore = numLowPriorityWrites(recipient);

assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: donor.shardName, _waitForDelete: true}));

lowPriorityAfter = numLowPriorityWrites(recipient);

assert.eq(
    lowPriorityAfter,
    lowPriorityBefore,
    "Should NOT see low-priority writes when executionControlDeprioritizeBackgroundTasks is disabled",
);

st.stop();
