/*
 * Test that resharding works on unsplittable collections.
 * @tags: [
 *   featureFlagTrackUnshardedCollectionsOnShardingCatalog,
 *   featureFlagReshardingImprovements,
 *   multiversion_incompatible,
 *   assumes_balancer_off,
 * ]
 */

const kDbName = "test";

const st = new ShardingTest({shards: 2});
const mongos = st.s;
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;

const kDataColl = 'unsplittable_collection_resharding';
const kDataCollNss = kDbName + '.' + kDataColl;
const kNumObjs = 3;

assert.commandWorked(st.s.adminCommand({enableSharding: kDbName, primaryShard: shard0}));
assert.commandWorked(st.s.getDB(kDbName).runCommand({createUnsplittableCollection: kDataColl}));

for (let i = 0; i < kNumObjs; ++i) {
    st.s.getCollection(kDataCollNss).insert({x: i});
}

assert.eq(kNumObjs, st.rs0.getPrimary().getCollection(kDataCollNss).countDocuments({}));

assert.commandWorked(st.s.adminCommand({
    reshardCollection: kDataCollNss,
    key: {_id: 1},
    forceRedistribution: true,
    shardDistribution: [{shard: shard1, min: {_id: MinKey}, max: {_id: MaxKey}}]
}));

assert.eq(kNumObjs, st.rs1.getPrimary().getCollection(kDataCollNss).countDocuments({}));
assert.eq(0, st.rs0.getPrimary().getCollection(kDataCollNss).countDocuments({}));
// st.s.getDB(kDbName).dropDatabase();

st.stop();
