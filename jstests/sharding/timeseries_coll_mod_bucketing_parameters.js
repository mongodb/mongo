/**
 * Test $collMod command on a sharded timeseries collection. This test specifically targets the
 * manipulation of the bucketing parameters which are made up of: granularity, bucketMaxSpanSeconds,
 * and bucketRoundingSeconds.
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

const getConfigGranularity = function(db) {
    return db.getSiblingDB('config')
        .collections.findOne({_id: bucketNss})
        .timeseriesFields.granularity;
};
const getConfigMaxSpanSeconds = function(db) {
    return db.getSiblingDB('config')
        .collections.findOne({_id: bucketNss})
        .timeseriesFields.bucketMaxSpanSeconds;
};
const getConfigRoundingSeconds = function(db) {
    return db.getSiblingDB('config')
        .collections.findOne({_id: bucketNss})
        .timeseriesFields.bucketRoundingSeconds;
};

const checkConfigParametersAfterCollMod = function() {
    jsTestLog("Entering checkConfigParametersAfterCollMod...");

    const st = new ShardingTest({shards: 1, rs: {nodes: 1}});
    const mongos = st.s0;
    const db = mongos.getDB(dbName);

    if (!TimeseriesTest.shardedtimeseriesCollectionsEnabled(st.shard0)) {
        jsTestLog(
            "Skipping test because the sharded time-series collection feature flag is disabled");
        st.stop();
        return;
    }

    if (!TimeseriesTest.timeseriesScalabilityImprovementsEnabled(st.shard0)) {
        jsTestLog("Skipping test because the timeseries scalability feature flag is disabled");
        st.stop();
        return;
    }

    // Create and shard the time-series collection.
    assert.commandWorked(
        db.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}));
    assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
    assert.commandWorked(mongos.adminCommand({
        shardCollection: viewNss,
        key: {[metaField]: 1},
    }));

    // The default granularity for a time-series collection is 'seconds', so verify that alligns
    // with the information on the config server.
    let currentMaxSpan = TimeseriesTest.getBucketMaxSpanSecondsFromGranularity('seconds');
    assert.eq(getConfigGranularity(db), 'seconds');
    assert.eq(getConfigMaxSpanSeconds(db), currentMaxSpan);
    assert.isnull(getConfigRoundingSeconds(db));

    // 1. Modify the bucketMaxSpanSeconds and bucketRoundingSeconds to the maxSpanSeconds for
    // seconds. We expect this to succeed and we should see that the granularity is null and the new
    // values should be present on the config server.
    assert.commandWorked(db.runCommand({
        collMod: collName,
        timeseries: {bucketMaxSpanSeconds: currentMaxSpan, bucketRoundingSeconds: currentMaxSpan}
    }));
    assert.isnull(getConfigGranularity(db));
    assert.eq(getConfigMaxSpanSeconds(db), currentMaxSpan);
    assert.eq(getConfigRoundingSeconds(db), currentMaxSpan);

    // 2. Change the granularity to 'minutes'. We expect this to succeed and the values on the
    // config server should reflect the values which correspond to the 'minutes' granularity.
    // Note: when specifying a granularity, the bucketRoundingSeconds parameter should NOT be
    // present on the config server (it is calculated based off of the granularity).
    currentMaxSpan = TimeseriesTest.getBucketMaxSpanSecondsFromGranularity('minutes');
    assert.commandWorked(db.runCommand({collMod: collName, timeseries: {granularity: 'minutes'}}));
    assert.eq(getConfigGranularity(db), 'minutes');
    assert.eq(getConfigMaxSpanSeconds(db), currentMaxSpan);
    assert.isnull(getConfigRoundingSeconds(db));

    // 3. Modify the bucketMaxSpanSeconds and bucketRoundingSeconds to a custom value and check the
    // config server for consistency.
    currentMaxSpan = TimeseriesTest.getBucketMaxSpanSecondsFromGranularity('minutes');
    assert.commandWorked(db.runCommand({
        collMod: collName,
        timeseries: {bucketMaxSpanSeconds: currentMaxSpan, bucketRoundingSeconds: currentMaxSpan}
    }));
    assert.isnull(getConfigGranularity(db));
    assert.eq(getConfigMaxSpanSeconds(db), currentMaxSpan);
    assert.eq(getConfigRoundingSeconds(db), currentMaxSpan);

    // 4. Change the granularity to 'hours' and check the config server for consistency.
    currentMaxSpan = TimeseriesTest.getBucketMaxSpanSecondsFromGranularity('hours');
    assert.commandWorked(db.runCommand({collMod: collName, timeseries: {granularity: 'hours'}}));
    assert.eq(getConfigGranularity(db), 'hours');
    assert.eq(getConfigMaxSpanSeconds(db), currentMaxSpan);
    assert.isnull(getConfigRoundingSeconds(db));

    // 5. We expect changing the granularity to a value lower than 'hours' will fail.
    assert.commandFailedWithCode(
        db.runCommand({collMod: collName, timeseries: {granularity: 'minutes'}}),
        ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(
        db.runCommand({collMod: collName, timeseries: {granularity: 'seconds'}}),
        ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(db.runCommand({
        collMod: collName,
        timeseries:
            {bucketMaxSpanSeconds: currentMaxSpan - 1, bucketRoundingSeconds: currentMaxSpan - 1}
    }),
                                 ErrorCodes.InvalidOptions);
    // Make sure that the bucketing parameters are unchanged.
    assert.eq(getConfigGranularity(db), 'hours');
    assert.eq(getConfigMaxSpanSeconds(db), currentMaxSpan);
    assert.isnull(getConfigRoundingSeconds(db));

    // 6. Modify the bucketMaxSpanSeconds and bucketRoundingSeconds to a custom value and check the
    // config server for consistency.
    assert.commandWorked(db.runCommand({
        collMod: collName,
        timeseries: {bucketMaxSpanSeconds: currentMaxSpan, bucketRoundingSeconds: currentMaxSpan}
    }));
    assert.isnull(getConfigGranularity(db));
    assert.eq(getConfigMaxSpanSeconds(db), currentMaxSpan);
    assert.eq(getConfigRoundingSeconds(db), currentMaxSpan);

    st.stop();
    jsTestLog("Exiting checkConfigParametersAfterCollMod.");
};

const checkShardRoutingAfterCollMod = function() {
    jsTestLog("Entering checkShardRoutingAfterCollMod...");

    const st = new ShardingTest({shards: 2, rs: {nodes: 2}, mongos: 2});
    const mongos0 = st.s0;
    const mongos1 = st.s1;
    const shard0 = st.shard0;
    const shard1 = st.shard1;
    const db = mongos0.getDB(dbName);

    if (!TimeseriesTest.timeseriesScalabilityImprovementsEnabled(mongos0)) {
        jsTestLog(
            "Skipped test as the featureFlagTimeseriesScalabilityImprovements feature flag is not enabled.");
        st.stop();
        return;
    }

    // Create and shard a time-series collection using custom bucketing parameters.
    assert.commandWorked(db.createCollection(collName, {
        timeseries: {
            timeField: timeField,
            metaField: metaField,
            bucketMaxSpanSeconds: 60,
            bucketRoundingSeconds: 60
        }
    }));
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

    // Based on a bucketMaxSpan of 60, the time document will be routed to shard1 through mongos0.
    const time = ISODate('2022-01-01 10:30:50');
    assert.commandWorked(
        mongos0.getDB(dbName).getCollection(collName).insert({_id: 1, [timeField]: time}));
    assertDocumentOnShard(shard1, 1);

    const failPoint = configureFailPoint(shard0.getDB(dbName), "collModBeforeConfigServerUpdate");
    const parallelGranularityUpdate = startParallelShell(
        funWithArgs(function(dbName, collName) {
            assert.commandWorked(db.getSiblingDB(dbName).runCommand({
                collMod: collName,
                timeseries: {bucketMaxSpanSeconds: 86400, bucketRoundingSeconds: 86400}
            }));
        }, dbName, collName), mongos0.port);

    failPoint.wait();

    // While the collMod command on the config server is still being processed, inserts on the
    // collection should be blocked.
    assert.commandFailedWithCode(
        mongos0.getDB(dbName).runCommand(
            {insert: collName, documents: [{[timeField]: ISODate()}], maxTimeMS: 2000}),
        ErrorCodes.MaxTimeMSExpired);
    assert.commandFailedWithCode(
        mongos0.getDB(dbName).runCommand({find: collName, maxTimeMS: 2000}),
        ErrorCodes.MaxTimeMSExpired);

    failPoint.off();
    parallelGranularityUpdate();

    // Based on a bucketMaxSpan of 86400, the time document will be routed to shard1 through
    // mongos0.
    assert.commandWorked(
        mongos0.getDB(dbName).getCollection(collName).insert({_id: 2, [timeField]: time}));
    assertDocumentOnShard(shard0, 2);

    // Assert that a collection with a bucketMaxSpan of 86400 will find both documents through
    // mongos1.
    assert.eq(mongos1.getDB(dbName).getCollection(collName).countDocuments({[timeField]: time}), 2);
    assert.eq(0, st.config.collections.countDocuments({allowMigrations: {$exists: true}}));

    st.stop();
    jsTestLog("Exiting checkShardRoutingAfterCollMod.");
};

checkConfigParametersAfterCollMod();

checkShardRoutingAfterCollMod();
})();
