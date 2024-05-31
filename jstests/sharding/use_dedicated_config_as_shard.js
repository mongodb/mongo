/**
 * This branch is still using the ConfigServerCatalogCacheLoader on config servers if
 * featureFlagTransitionToCatalogShard is disabled, despite PM-2290 being present (see
 * SERVER-84548). There were some edge cases missed:
 * 1. Calling enableSharding with config as the primary shard would fail,
 * but still add an entry to config.databases with primary: config. This could eventually lead the
 * config server to crash if e.g. a collection belonging to this database was later dropped (see
 * BF-32209).
 * 2. Calling moveChunk/moveRange with config as the destination shard would crash the config
 * server (see BF-33254).
 *
 * This tests verifies that those scenarios do not result in crashes.
 */

load("jstests/libs/feature_flag_util.js");
load("jstests/sharding/libs/create_sharded_collection_util.js");

const dbName = 'test';
const collName = 'coll';
const namespace = `${dbName}.${collName}`;

const st = new ShardingTest({mongos: 1, shards: 2});

(() => {
    if (FeatureFlagUtil.isPresentAndEnabled(st.configRS.getPrimary(), "TransitionToCatalogShard")) {
        jsTestLog("Skipping test because featureFlagTransitionToCatalogShard is enabled");
        return;
    }

    assert.commandFailedWithCode(
        st.s.adminCommand({enableSharding: dbName, primaryShard: "config"}),
        ErrorCodes.ShardNotFound);
    assert.eq(st.s.getDB("config").databases.find({primary: "config"}).toArray(), []);

    CreateShardedCollectionUtil.shardCollectionWithChunks(
        st.s.getDB(dbName).getCollection(collName), {_id: 1}, [
            {min: {_id: MinKey}, max: {_id: 10}, shard: st.shard0.shardName},
            {min: {_id: 10}, max: {_id: MaxKey}, shard: st.shard1.shardName},
        ]);

    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: namespace, to: "config", find: {_id: 0}}),
        ErrorCodes.ShardNotFound);
})();

st.stop();
