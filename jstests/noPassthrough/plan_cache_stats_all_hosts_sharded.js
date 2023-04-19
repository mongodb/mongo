// Tests that the $planCacheStats will collect data from all nodes in a shard.
//
// @tags: [
//   assumes_read_concern_unchanged,
//   assumes_read_preference_unchanged,
//   # TODO SERVER-67607: Test plan cache with CQF enabled.
//   cqf_incompatible,
// ]
(function() {
"use strict";

load("jstests/sharding/libs/create_sharded_collection_util.js");

for (let shardCount = 1; shardCount <= 2; shardCount++) {
    const st = new ShardingTest({name: jsTestName(), shards: shardCount, rs: {nodes: 2}});

    const db = st.s.getDB("test");
    const coll = db.plan_cache_stats_all_servers;
    coll.drop();
    const planCache = coll.getPlanCache();

    CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {a: 1}, [
        {min: {a: MinKey}, max: {a: 5}, shard: st.shard0.shardName},
        {min: {a: 5}, max: {a: MaxKey}, shard: st["shard" + (1 % shardCount).toString()].shardName},
    ]);

    assert.commandWorked(coll.createIndex({b: 1}));
    assert.commandWorked(coll.createIndex({c: 1}));
    assert.commandWorked(coll.insertOne({a: 1, b: 2, c: 3}));
    assert.commandWorked(coll.insertOne({a: 11, b: 12, c: 13}));

    planCache.clear();

    // Send single shard request to primary node.
    assert.eq(1, coll.find({a: 1, b: 2}).readPref("primary").itcount());
    // Send multi shard request to secondary nodes.
    assert.eq(1, coll.find({b: 12, c: 13}).readPref("secondary").itcount());

    // On primary there is only one plan in the plan cache, because the query was sent to a single
    // shard
    db.getMongo().setReadPref("primary");
    assert.eq(1, coll.aggregate({$planCacheStats: {}}).itcount());
    // On secondaries there is a plan for each shard
    db.getMongo().setReadPref("secondary");
    assert.eq(shardCount, coll.aggregate({$planCacheStats: {}}).itcount());

    // If we set allHosts: true, we return all plans despite any read preference setting.
    const totalPlans = 1 + shardCount;
    db.getMongo().setReadPref("primary");
    assert.eq(totalPlans, coll.aggregate({$planCacheStats: {allHosts: true}}).itcount());
    db.getMongo().setReadPref("secondary");
    assert.eq(totalPlans, coll.aggregate({$planCacheStats: {allHosts: true}}).itcount());

    st.stop();
}
}());
