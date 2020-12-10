/**
 * Tests that chunk migrations are prohibited on a collection that is undergoing a resharding
 * operation.
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

const db = 'reshardingDb';
const col = 'coll';
const ns = `${db}.${col}`;
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

const mongos = sourceCollection.getMongo();
const tempns = `${db}.${reshardingTest.temporaryReshardingCollectionName}`;
assert.soon(() => {
    return mongos.getDB("config").collections.findOne({_id: ns}).allowMigrations === false;
});
assert.soon(() => {
    return mongos.getDB("config").collections.findOne({_id: tempns}).allowMigrations === false;
});

assert.commandFailedWithCode(
    mongos.adminCommand({moveChunk: ns, find: {oldKey: -10}, to: donorShardNames[1]}),
    ErrorCodes.ConflictingOperationInProgress);

reshardingTest.teardown();
})();
