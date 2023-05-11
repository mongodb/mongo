/**
 * Test the correctness of multiple deletes during resharding with a reduced ticket pool size.
 *
 * @tags: [
 *  requires_sharding,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

const kNumWriteTickets = 5;
const kReshardingOplogBatchTaskCount = 20;
const reshardingTest = new ReshardingTest({
    wiredTigerConcurrentWriteTransactions: kNumWriteTickets,
    reshardingOplogBatchTaskCount: kReshardingOplogBatchTaskCount
});

reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});
for (let i = 0; i < 100; i++) {
    assert.commandWorked(sourceCollection.insert([{x: 1}]));
}
assert.commandWorked(sourceCollection.insert([{x: 3}, {x: 3}]));
const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const coordinator = new Mongo(topology.configsvr.nodes[0]);
const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        // We wait until cloneTimestamp has been chosen to guarantee that any subsequent writes will
        // be applied by the ReshardingOplogApplier.
        reshardingTest.awaitCloneTimestampChosen();
        assert.commandWorked(sourceCollection.remove({x: 1}, {justOne: false}));
    });
reshardingTest.teardown();
})();
