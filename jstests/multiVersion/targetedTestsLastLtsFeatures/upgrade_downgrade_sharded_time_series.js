/**
 * Verify the behaviour of time-series collections during the upgrade and downgrade process.
 *  @tags: [
 *   requires_fcv_51
 *  ]
 */

(function() {
'use strict';

load('jstests/libs/uuid_util.js');                   // For extractUUIDFromObject.
load('jstests/multiVersion/libs/multi_cluster.js');  // For upgradeCluster.
load('jstests/core/timeseries/libs/timeseries.js');  // For timeseries object.

Random.setRandomSeed();

const dbName = 'testDB';
const collTSName = 'testTS';
const timeField = 'time';
const metaField = 'hostid';
const collName = 'test';
let currentId = 0;

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
            mongodOptions['setParameter'] = "featureFlagShardedTimeSeries=true";
        } else {
            configSvr.savedOptions.setParameter = {};  // Resetting the sharded time series flag.
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
            mongosOptions['setParameter'] = "featureFlagShardedTimeSeries=true";
        } else {
            mongos.savedOptions.setParameter = {};  // Resetting the sharded time series flag.
        }
        mongos = MongoRunner.runMongos(mongosOptions);
        st["s" + i] = st._mongos[i] = mongos;
        if (i == 0)
            st.s = mongos;
    }

    st.config = st.s.getDB("config");
    st.admin = st.s.getDB("admin");

    st._rs.forEach((rs) => {
        if (enableShardedTimeSeriesFeature) {
            // Save the node parameters before adding the sharded time series flag.
            ltsNodeParameterSet = JSON.parse(JSON.stringify(rs.nodes[0].fullOptions.setParameter));
            rs.test.upgradeSet(
                {binVersion: binVersion, setParameter: "featureFlagShardedTimeSeries=true"});
        } else {
            // Resetting node parameters.
            rs.nodes.forEach((node) => {
                node.fullOptions.setParameter = ltsNodeParameterSet;
            });
            rs.test.nodeOptions.n0.setParameter = {};
            rs.test.nodeOptions.n1.setParameter = {};
            rs.test.upgradeSet({binVersion: binVersion});
        }
    });

    st.upgradeCluster(binVersion,
                      {upgradeMongos: false, upgradeShards: false, upgradeCluster: false});
}

function getNodeName(node) {
    const info = node.adminCommand({hello: 1});
    return info.setName + '_' + (info.secondary ? 'secondary' : 'primary');
}

function checkConfigAndShardsFCV(expectedFCV) {
    const configPrimary = st.configRS.getPrimary();

    const shard0Primary = st.rs0.getPrimary();
    const shard0Secondary = st.rs0.getSecondary();
    shard0Secondary.setSecondaryOk();

    const shard1Primary = st.rs1.getPrimary();
    const shard1Secondary = st.rs1.getSecondary();
    shard1Secondary.setSecondaryOk();

    for (const node
             of [configPrimary, shard0Primary, shard0Secondary, shard1Primary, shard1Secondary]) {
        jsTest.log('Verify that the FCV is properly set on node ' + getNodeName(node));

        const fcvDoc = node.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
        assert.eq(expectedFCV, fcvDoc.featureCompatibilityVersion.version);
    }
}

function createShardedCollection() {
    assert.commandWorked(
        st.s.adminCommand({shardCollection: `${dbName}.${collName}`, key: {x: 1}}));

    const coll = st.s.getCollection(`${dbName}.${collName}`);
    assert.commandWorked(coll.insert([{x: -1}, {x: 1}]));

    assert.commandWorked(st.s.adminCommand({split: `${dbName}.${collName}`, middle: {x: 0}}));
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: `${dbName}.${collName}`, find: {x: 1}, to: st.shard1.shardName}));
}

function testShardedCollection() {
    const coll = st.s.getCollection(`${dbName}.${collName}`);
    const documentsCount = coll.find().count();

    assert.commandWorked(coll.insert([{x: -2}, {x: 2}]));
    assert.eq(coll.find().count(), documentsCount + 2);

    assert.commandWorked(coll.remove({x: -2}));
    assert.commandWorked(coll.remove({x: 2}));
    assert.eq(coll.find().count(), documentsCount);
}

function generateDoc(time, metaValue) {
    return TimeseriesTest.generateHosts(1).map((host, index) => Object.assign(host, {
        _id: currentId++,
        [metaField]: metaValue,
        [timeField]: ISODate(time),
    }))[0];
}

function insertDocument(timestamp, metaValue) {
    const doc = generateDoc(timestamp, metaValue);
    assert.commandWorked(st.s.getDB(dbName).getCollection(collTSName).insert(doc));
}

function createShardedTimeSeries() {
    // Shard time-series collection.
    const shardKey = {[timeField]: 1};
    assert.commandWorked(st.s.getDB(dbName).createCollection(
        collTSName, {timeseries: {timeField: timeField, granularity: "hours"}}));
    assert.commandWorked(st.s.adminCommand({
        shardCollection: `${dbName}.${collTSName}`,
        key: shardKey,
    }));

    // Split the chunks such that primary shard has chunk: [MinKey, 2020-01-01) and other shard has
    // chunk [2020-01-01, MaxKey].
    let splitPoint = {[`control.min.${timeField}`]: ISODate(`2020-01-01`)};
    assert.commandWorked(
        st.s.adminCommand({split: `${dbName}.system.buckets.${collTSName}`, middle: splitPoint}));

    // Move one of the chunks into the second shard.
    const primaryShard = st.getPrimaryShard(dbName);
    const otherShard = st.getOther(primaryShard);
    assert.commandWorked(st.s.adminCommand({
        movechunk: `${dbName}.system.buckets.${collTSName}`,
        find: splitPoint,
        to: otherShard.name,
        _waitForDelete: true
    }));

    // Ensure that each shard owns one chunk.
    const counts = st.chunkCounts(`system.buckets.${collTSName}`, dbName);
    assert.eq(1, counts[primaryShard.shardName], counts);
    assert.eq(1, counts[otherShard.shardName], counts);

    for (let i = 0; i < 2; i++) {
        insertDocument("2019-11-11");
        insertDocument("2019-12-31");
        insertDocument("2020-01-21");
        insertDocument("2020-11-31");
    }
}

function testEnabledShardedTimeSeriesSupport() {
    const coll = st.s.getDB(dbName).getCollection(collTSName);
    const result = coll.find({time: {"$eq": ISODate("2020-01-21")}}).count();
    assert.eq(result, 2);
}

function testDisabledShardedTimeSeriesSupport() {
    assert.commandFailedWithCode(st.s.getDB(dbName).runCommand(
                                     {aggregate: collTSName, pipeline: [{$match: {}}], cursor: {}}),
                                 238);
}

function checkClusterBeforeUpgrade(fcv) {
    checkConfigAndShardsFCV(fcv);
    testShardedCollection();

    // Test that sharding time series fails before upgrade.
    const shardKey = {[timeField]: 1};
    const testTS = "testTS2";
    assert.commandWorked(st.s.getDB(dbName).createCollection(
        testTS, {timeseries: {timeField: timeField, granularity: "hours"}}));
    assert.commandFailedWithCode(st.s.adminCommand({
        shardCollection: `${dbName}.${testTS}`,
        key: shardKey,
    }),
                                 5731502);
}

function checkClusterAfterUpgrade(fcv) {
    checkConfigAndShardsFCV(fcv);
    testShardedCollection();
    testEnabledShardedTimeSeriesSupport(dbName);
}

function checkClusterAfterDowngrade(fcv) {
    checkConfigAndShardsFCV(fcv);
    testShardedCollection();
    testDisabledShardedTimeSeriesSupport(dbName);
}

// Set and test cluster using old binaries in default FCV mode.
jsTest.log('Deploying cluster version 5.0');
const st = setupClusterAndDatabase('5.0');
if (!TimeseriesTest.shardedtimeseriesCollectionsEnabled(st.shard0)) {
    // Create non time series sharded collection.
    createShardedCollection();
    checkClusterBeforeUpgrade('5.0');

    // Set and test cluster using latest binaries in latest FCV mode
    upgradeBinary('latest', true);
    jsTest.log('Upgrading FCV to ' + latestFCV);
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Create time series sharded collection.
    createShardedTimeSeries();
    checkClusterAfterUpgrade('5.1');

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: '5.0'}));
    upgradeBinary('5.0', false);
    checkClusterAfterDowngrade('5.0');

    upgradeBinary('latest', true);
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
}
st.stop();
})();
