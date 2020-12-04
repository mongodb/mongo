/**
 * Test for the ReshardingTest fixture itself.
 *
 * Verifies that the reshardCollection command is run and kept suspended in the "applying" state.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
reshardingTest.setup();

const ns = "reshardingDb.coll";
const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

// Perform some inserts before resharding starts so there's data to clone.
assert.commandWorked(sourceCollection.insert(
    [
        {_id: "stays on shard0", oldKey: -10, newKey: -10},
        {_id: "moves to shard0", oldKey: 10, newKey: -10},
        {_id: "moves to shard1", oldKey: -10, newKey: 10},
        {_id: "stays on shard1", oldKey: 10, newKey: 10},
    ],
    {writeConcern: {w: "majority"}}));

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.startReshardingInBackground({
    newShardKeyPattern: {newKey: 1},
    newChunks: [
        {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
        {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
    ],
});

// Wait for the recipients to have finished cloning and then perform some updates so there's oplog
// entries to fetch and apply.
const mongos = sourceCollection.getMongo();
assert.soon(() => {
    const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne();
    return coordinatorDoc !== null && coordinatorDoc.state === "applying";
});

assert.commandWorked(sourceCollection.update({_id: 0}, {$inc: {extra: 1}}, {multi: true}));

// The reshardCollection command should still be actively running on mongos.
const ops = mongos.getDB("admin")
                .aggregate([
                    {$currentOp: {allUsers: true, localOps: true}},
                    {$match: {"command.reshardCollection": ns}},
                ])
                .toArray();
assert.eq(1, ops.length, "failed to find reshardCollection in $currentOp output");

reshardingTest.teardown();
})();
