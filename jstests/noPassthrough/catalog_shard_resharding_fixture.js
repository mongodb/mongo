/**
 * Test the ReshardingTest fixture can work with a catalogShard.
 *
 * @tags: [
 *   requires_fcv_70,
 *   featureFlagCatalogShard,
 *   featureFlagTransitionToCatalogShard,
 * ]
 */

(function() {
"use strict";

load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest =
    new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true, catalogShard: true});
reshardingTest.setup();

const ns = "reshardingDb.coll";
const donorShardNames = reshardingTest.donorShardNames;
assert.includes(donorShardNames, "config");
const sourceCollection = reshardingTest.createShardedCollection({
    ns,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

// Perform some inserts before resharding starts so there's data to clone.
const docs = [
    {_id: "stays on shard0", oldKey: -10, newKey: -10},
    {_id: "moves to shard0", oldKey: 10, newKey: -10},
    {_id: "moves to shard1", oldKey: -10, newKey: 10},
    {_id: "stays on shard1", oldKey: 10, newKey: 10}
];
assert.commandWorked(sourceCollection.insert(docs));

const recipientShardNames = reshardingTest.recipientShardNames;
assert.includes(recipientShardNames, "config");
reshardingTest.withReshardingInBackground({
    newShardKeyPattern: {newKey: 1},
    newChunks: [
        {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
        {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
    ],
});

// All documents should still be visible.
assert.sameMembers(sourceCollection.find().toArray(), docs);

reshardingTest.teardown();
})();
