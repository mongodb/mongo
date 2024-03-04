// Tests that the $planCacheStats will collect data from all nodes in a shard.
//
// @tags: [
//   assumes_read_concern_unchanged,
//   assumes_read_preference_unchanged,
//   # TODO SERVER-85728: Enable Bonsai plan cache tests involving indices.
//   cqf_experimental_incompatible,
// ]
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

for (let shardCount = 1; shardCount <= 2; shardCount++) {
    // It is essential to disable mirroredReads to avoid having more plans in
    // secondary node's plan cache than expected. Without disabling it, this test will
    // inconsistently fail in burn_in_tests.
    const st = new ShardingTest({
        name: jsTestName(),
        shards: shardCount,
        rs: {nodes: 2, setParameter: {mirrorReads: tojson({samplingRate: 0.0})}}
    });
    st.waitForShardingInitialized();

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
    // Wait for all operations to fully replicate on all shards.
    st.awaitReplicationOnShards();

    planCache.clear();
    assert.eq(0, coll.aggregate({$planCacheStats: {}}).itcount());

    assert.eq(
        2, st.shard0.rs.getReplSetConfig()["members"].length, st.shard0.rs.getReplSetConfig());
    // This ensure that secondaries have identified their roles before executing the queries.
    // Sending a query with secondary read pref without this wait could result in an error.
    st.shard0.rs.awaitSecondaryNodes();

    if (shardCount === 2) {
        assert.eq(
            2, st.shard1.rs.getReplSetConfig()["members"].length, st.shard1.rs.getReplSetConfig());
        st.shard1.rs.awaitSecondaryNodes();
    }

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
    assert.eq(shardCount,
              coll.aggregate({$planCacheStats: {}}).itcount(),
              coll.aggregate([{$planCacheStats: {}}]).toArray());

    // If we set allHosts: true, we return all plans despite any read preference setting.
    const totalPlans = 1 + shardCount;
    db.getMongo().setReadPref("primary");
    assert.eq(totalPlans, coll.aggregate({$planCacheStats: {allHosts: true}}).itcount());
    db.getMongo().setReadPref("secondary");
    assert.eq(totalPlans, coll.aggregate({$planCacheStats: {allHosts: true}}).itcount());

    st.stop();
}
