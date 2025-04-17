/*
 * Tests basic movePrimary followed by reshardCollection such that the shard which becomes the
 * primary after movePrimary operation doesn't own any chunks under reshardCollection. This
 * test covers SERVER-86622.
 *
 */
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 1, reshardInPlace: false});
reshardingTest.setup();
const collName = "reshardingDb.coll";
// shard1-recipient-0
const recipientShardName = reshardingTest.recipientShardNames[0];
// shard0-donor-0
const donorShardName = reshardingTest.donorShardNames[0];

const sourceCollection = reshardingTest.createShardedCollection({
    ns: collName,
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: recipientShardName}],
    primaryShardName: recipientShardName,
});

const originalCollInfo = sourceCollection.exists();
assert.neq(originalCollInfo, null, "failed to find sharded collection before resharding");

const st = reshardingTest._st;
// Move chunk to and from donorShard to warm up config server CatalogCache
assert.commandWorked(
    st.s.adminCommand({moveChunk: collName, find: {oldKey: 0}, to: donorShardName}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: collName, find: {oldKey: 0}, to: recipientShardName}));

assert.commandWorked(st.s.adminCommand({movePrimary: "reshardingDb", to: donorShardName}));

// The ReshardingTest fixture maintains stale info about the primary shard from what is passed in
// createShardedCollection above post movePrimary command that updates the primary shard to
// donorShardName. This needs to be updated to perform consistency checks correctly post resharding.
reshardingTest.updatePrimaryShard(donorShardName);
reshardingTest.withReshardingInBackground({
    newShardKeyPattern: {newKey: 1},
    newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardName}],
});

assert(st.shard1.getCollection(collName).exists(),
       "Collection does not exist on shard1-recipient0 despite the shard having chunks");
const newCollInfo = sourceCollection.exists();

// There should be a catalog entry present in new primary shard
assert.neq(newCollInfo, null, "failed to find sharded collection after resharding");

reshardingTest.teardown();
