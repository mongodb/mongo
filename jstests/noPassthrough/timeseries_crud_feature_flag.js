/**
 * Test CRUD operations with sharded time-series feature flag off.
 *
 * @tags: [
 *   requires_fcv_50,
 *   requires_sharding,
 *   requires_find_command,
 * ]
 */

(function() {
'use strict';

load('jstests/libs/uuid_util.js');                   // For extractUUIDFromObject.
load('jstests/multiVersion/libs/multi_cluster.js');  // For upgradeCluster.
load('jstests/core/timeseries/libs/timeseries.js');  // For timeseries object.

Random.setRandomSeed();

const dbName = 'testDb';
const collName = 'testColl';
const timeField = 'time';
const metaField = 'meta';
const bucketNss = `${dbName}.system.buckets.${collName}`;

function setupClusterAndDatabase(binVersion) {
    const st = new ShardingTest({
        mongos: 1,
        config: 1,
        shards: 2,
        other: {
            mongosOptions: {binVersion: binVersion},
            configOptions: {binVersion: binVersion},
            shardOptions: {binVersion: binVersion},
            rs: {nodes: 2}
        }
    });
    st.configRS.awaitReplication();

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    return st;
}

let ltsNodeParameterSet;
function upgradeBinary(binVersion, enableShardedTimeSeriesFeature) {
    const numConfigs = st.configRS.nodes.length;

    for (let i = 0; i < numConfigs; i++) {
        let configSvr = st.configRS.nodes[i];

        MongoRunner.stopMongod(configSvr);
        const mongodOptions = {restart: configSvr, binVersion: binVersion, appendOptions: true};
        if (enableShardedTimeSeriesFeature) {
            mongodOptions.setParameter = {
                featureFlagShardedTimeSeries: true,
                featureFlagTimeseriesUpdatesAndDeletes: true
            };
        } else {
            configSvr.savedOptions.setParameter = {
                featureFlagTimeseriesUpdatesAndDeletes: true
            };  // Resetting the sharded time series flag.
        }
        configSvr = MongoRunner.runMongod(mongodOptions);
        st["config" + i] = st["c" + i] = st.configRS.nodes[i] = configSvr;
    }

    let numMongoses = st._mongos.length;
    for (let i = 0; i < numMongoses; i++) {
        let mongos = st._mongos[i];
        MongoRunner.stopMongos(mongos);
        const mongosOptions = {restart: mongos, binVersion: binVersion, appendOptions: true};
        if (enableShardedTimeSeriesFeature) {
            mongosOptions.setParameter = {
                featureFlagShardedTimeSeries: true,
                featureFlagTimeseriesUpdatesAndDeletes: true
            };
        } else {
            mongos.savedOptions.setParameter = {
                featureFlagTimeseriesUpdatesAndDeletes: true
            };  // Resetting the sharded time series flag.
        }
        mongos = MongoRunner.runMongos(mongosOptions);
        st["s" + i] = st._mongos[i] = mongos;
        if (i === 0)
            st.s = mongos;
    }

    st.config = st.s.getDB("config");
    st.admin = st.s.getDB("admin");

    st._rs.forEach((rs) => {
        if (enableShardedTimeSeriesFeature) {
            // Save the node parameters before adding the sharded time series flag.
            ltsNodeParameterSet = JSON.parse(JSON.stringify(rs.nodes[0].fullOptions.setParameter));
            rs.test.upgradeSet({
                binVersion: binVersion,
                setParameter: {
                    featureFlagShardedTimeSeries: true,
                    featureFlagTimeseriesUpdatesAndDeletes: true
                }
            });
        } else {
            // Resetting node parameters.
            rs.nodes.forEach((node) => {
                node.fullOptions.setParameter = Object.assign(
                    {}, ltsNodeParameterSet, {featureFlagTimeseriesUpdatesAndDeletes: true});
            });
            rs.test.nodeOptions.n0.setParameter = {featureFlagTimeseriesUpdatesAndDeletes: true};
            rs.test.nodeOptions.n1.setParameter = {featureFlagTimeseriesUpdatesAndDeletes: true};
            rs.test.upgradeSet({binVersion: binVersion});
        }
    });

    st.upgradeCluster(binVersion,
                      {upgradeMongos: false, upgradeShards: false, upgradeCluster: false});
}

function splitAndMoveChunk(splitPoint) {
    const primaryShard = st.getPrimaryShard(dbName);
    const otherShard = st.getOther(primaryShard);
    assert.commandWorked(
        st.s.adminCommand({split: bucketNss, middle: {'control.min.time': splitPoint}}));
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: bucketNss, find: {'control.min.time': splitPoint}, to: otherShard.shardName}));
}

function createShardedTimeSeries() {
    jsTestLog("Creating a sharded time-series collection");

    // Create collection and insert data.
    let db = st.s.getDB(dbName);
    assert.commandWorked(db.createCollection(collName, {timeseries: {timeField, metaField}}));
    let coll = db.getCollection(collName);
    for (let i = 1; i <= 30; i++) {
        assert.commandWorked(coll.insert(
            {[timeField]: ISODate(`2021-09-${String(i).padStart(2, '0')}`), [metaField]: i}));
    }

    // Shard time-series collection.
    const shardKey = {[timeField]: 1};
    assert.commandWorked(coll.createIndex(shardKey));
    assert.commandWorked(st.s.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: shardKey,
    }));
}

function testCrud() {
    let db = st.s.getDB(dbName);
    let coll = db.getCollection(collName);

    // Query.
    assert.eq(30, coll.find().itcount());

    // Insertion.
    for (let i = 1; i <= 30; i++) {
        assert.commandWorked(coll.insert(
            {[timeField]: ISODate(`2021-11-${String(i).padStart(2, '0')}`), [metaField]: i}));
    }
    assert.eq(60, coll.find().itcount());

    // Update.
    assert.eq(0, coll.find({[metaField]: 0}).itcount());
    assert.commandWorked(db.runCommand({
        update: collName,
        updates: [{q: {[metaField]: 1}, u: {$set: {[metaField]: 0}}, multi: true}]
    }));
    assert.eq(2, coll.find({[metaField]: 0}).itcount());

    // Deletion.
    assert.eq(60, coll.find().itcount());
    assert.commandWorked(
        db.runCommand({delete: collName, deletes: [{q: {[metaField]: 0}, limit: 0}]}));
    assert.eq(58, coll.find().itcount());
}

const st = setupClusterAndDatabase('latest');

upgradeBinary('latest', true);

createShardedTimeSeries();

splitAndMoveChunk(ISODate('2021-10-30'));

upgradeBinary('latest', false);

splitAndMoveChunk(ISODate('2021-10-01'));

testCrud();

st.stop();
})();
