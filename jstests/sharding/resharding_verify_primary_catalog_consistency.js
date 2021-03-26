/**
 * Tests that the collection catalog entry is updated correctly and is consistent after a resharding
 * operation has completed.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest({numDonors: 2, reshardInPlace: false});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[1]}],
    primaryShardName: donorShardNames[0],
});

const originalCollInfo = sourceCollection.exists();
assert.neq(originalCollInfo, null, "failed to find sharded collection before resharding");

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withReshardingInBackground({
    newShardKeyPattern: {newKey: 1},
    newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
});

const newCollInfo = sourceCollection.exists();
assert.neq(newCollInfo, null, "failed to find sharded collection after resharding");
assert.neq(newCollInfo.info.uuid, originalCollInfo.info.uuid, {newCollInfo, originalCollInfo});

reshardingTest.teardown();
})();
