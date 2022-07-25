/**
 * Test $collMod command on a sharded timeseries collection.
 *
 * @tags: [
 *   requires_fcv_60,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");

const dbName = 'testDB';
const collName = 'testColl';
const timeField = 'tm';
const metaField = 'mt';
const indexName = 'index';
const viewNss = `${dbName}.${collName}`;
const bucketNss = `${dbName}.system.buckets.${collName}`;
const controlTimeField = `control.min.${timeField}`;

function runBasicTest() {
    const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
    const mongos = st.s0;
    const db = mongos.getDB(dbName);

    if (!TimeseriesTest.shardedtimeseriesCollectionsEnabled(st.shard0)) {
        jsTestLog(
            "Skipping test because the sharded time-series collection feature flag is disabled");
        st.stop();
        return;
    }

    assert.commandWorked(
        db.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}));

    // Updates for timeField and metaField are disabled.
    assert.commandFailedWithCode(db.runCommand({collMod: collName, timeseries: {timeField: 'x'}}),
                                 40415 /* Failed to parse */);
    assert.commandFailedWithCode(db.runCommand({collMod: collName, timeseries: {metaField: 'x'}}),
                                 40415 /* Failed to parse */);

    // Normal collMod commands works for the unsharded time-series collection.
    assert.commandWorked(db[collName].createIndex({[metaField]: 1}, {name: indexName}));
    assert.commandWorked(
        db.runCommand({collMod: collName, index: {name: indexName, hidden: true}}));
    assert.commandWorked(
        db.runCommand({collMod: collName, index: {name: indexName, hidden: false}}));

    // Granularity update works for unsharded time-series collection.
    assert.commandWorked(db.runCommand({collMod: collName, timeseries: {granularity: 'minutes'}}));

    // Shard the time-series collection.
    assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
    assert.commandWorked(mongos.adminCommand({
        shardCollection: viewNss,
        key: {[metaField]: 1},
    }));

    // Normal collMod commands works for the sharded time-series collection.
    assert.commandWorked(
        db.runCommand({collMod: collName, index: {name: indexName, hidden: true}}));
    assert.commandWorked(
        db.runCommand({collMod: collName, index: {name: indexName, hidden: false}}));

    // Granularity update works for sharded time-series collection, when we're using DDL
    // coordinator logic.
    const getGranularity = () => db.getSiblingDB('config')
                                     .collections.findOne({_id: bucketNss})
                                     .timeseriesFields.granularity;
    assert.eq(getGranularity(), 'minutes');
    assert.commandWorked(db.runCommand({collMod: collName, timeseries: {granularity: 'hours'}}));
    assert.eq(getGranularity(), 'hours');
    assert.eq(0, st.config.collections.countDocuments({allowMigrations: {$exists: true}}));
    st.stop();
}

function runReadAfterWriteTest() {
    const st = new ShardingTest({shards: 2, rs: {nodes: 2}, mongos: 2});
    const mongos0 = st.s0;
    const mongos1 = st.s1;
    const shard0 = st.shard0;
    const shard1 = st.shard1;
    const db = mongos0.getDB(dbName);

    const fcvResult = assert.commandWorked(
        shard0.getDB(dbName).adminCommand({getParameter: 1, featureCompatibilityVersion: 1}));
    if (MongoRunner.compareBinVersions(fcvResult.featureCompatibilityVersion.version, "6.0") < 0) {
        jsTestLog("FCV is less than 6.0, skip granularity update read after write test");
        return;
    }

    assert.commandWorked(db.createCollection(
        collName,
        {timeseries: {timeField: timeField, metaField: metaField, granularity: 'seconds'}}));
    st.ensurePrimaryShard(db.getName(), shard0.shardName);
    assert.commandWorked(mongos0.adminCommand({enableSharding: dbName}));
    assert.commandWorked(mongos0.adminCommand({
        shardCollection: viewNss,
        key: {[timeField]: 1},
    }));

    // Minkey --- 2022-01-01 09:00:00 --- MaxKey
    //       shard0                  shard1
    const splitChunk = {[controlTimeField]: ISODate('2022-01-01 09:00:00')};
    assert.commandWorked(mongos0.adminCommand({split: bucketNss, middle: splitChunk}));
    assert.commandWorked(mongos0.adminCommand(
        {moveChunk: bucketNss, find: splitChunk, to: shard1.name, _waitForDelete: true}));

    function assertDocumentOnShard(shard, _id) {
        const buckets =
            shard.getDB(dbName).getCollection(`system.buckets.${collName}`).find().toArray();
        const _ids = [];
        buckets.forEach(bucket => {
            for (let key in bucket.data._id) {
                _ids.push(bucket.data._id[key]);
            }
        });
        assert(_ids.some(x => x === _id));
    }

    // Based on 'seconds' granularity, the time document will be routed to shard1 through mongos0.
    const time = ISODate('2022-01-01 10:30:50');
    assert.commandWorked(
        mongos0.getDB(dbName).getCollection(collName).insert({_id: 1, [timeField]: time}));
    assertDocumentOnShard(shard1, 1);

    const failPoint = configureFailPoint(shard0.getDB(dbName), "collModBeforeConfigServerUpdate");
    const parallelGranularityUpdate =
        startParallelShell(funWithArgs(function(dbName, collName) {
                               assert.commandWorked(db.getSiblingDB(dbName).runCommand(
                                   {collMod: collName, timeseries: {granularity: 'hours'}}));
                           }, dbName, collName), mongos0.port);

    failPoint.wait();

    assert.commandFailedWithCode(
        mongos0.getDB(dbName).runCommand(
            {insert: collName, documents: [{[timeField]: ISODate()}], maxTimeMS: 2000}),
        ErrorCodes.MaxTimeMSExpired);
    assert.commandFailedWithCode(
        mongos0.getDB(dbName).runCommand({find: collName, maxTimeMS: 2000}),
        ErrorCodes.MaxTimeMSExpired);

    failPoint.off();
    parallelGranularityUpdate();

    // Based on 'hours' granularity, the time document will be routed to shard0 through mongos0.
    assert.commandWorked(
        mongos0.getDB(dbName).getCollection(collName).insert({_id: 2, [timeField]: time}));
    assertDocumentOnShard(shard0, 2);

    // Assert that we can use 'hours' granularity and find both documents through mongos1.
    assert.eq(mongos1.getDB(dbName).getCollection(collName).countDocuments({[timeField]: time}), 2);

    assert.eq(0, st.config.collections.countDocuments({allowMigrations: {$exists: true}}));
    st.stop();
}

runBasicTest();

runReadAfterWriteTest();
})();
