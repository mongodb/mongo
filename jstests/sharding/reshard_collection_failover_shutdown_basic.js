/**
 * Tests that reshardCollection succeeds when a participant experiences a failover or clean/unclean
 * restart during the operation.
 * Multiversion testing does not support tests that kill and restart nodes. So we had to add the
 * 'multiversion_incompatible' tag.
 * @tags: [
 *   uses_atclustertime,
 *   multiversion_incompatible,
 *   requires_persistence,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, enableElections: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

assert.commandWorked(sourceCollection.insert([
    {_id: "stays on shard0", oldKey: -10, newKey: -10},
    {_id: "moves to shard0", oldKey: 10, newKey: -10},
    {_id: "moves to shard1", oldKey: -10, newKey: 10},
    {_id: "stays on shard1", oldKey: 10, newKey: 10},
]));

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    (tempNs) => {
        reshardingTest.stepUpNewPrimaryOnShard(donorShardNames[0]);

        reshardingTest.killAndRestartPrimaryOnShard(recipientShardNames[0]);

        reshardingTest.shutdownAndRestartPrimaryOnShard(recipientShardNames[1]);
    });

reshardingTest.teardown();
})();
