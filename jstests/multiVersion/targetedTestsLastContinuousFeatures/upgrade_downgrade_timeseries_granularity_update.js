/**
 * Tests that granularity update for sharded time-series collections is disabled for FCV < 6.0
 */
(function() {
"use strict";

load('./jstests/multiVersion/libs/multi_cluster.js');

function runTest(downgradeVersion) {
    const st = new ShardingTest({shards: 1});
    const dbName = "test";
    const collName = jsTestName();
    const viewNss = `${dbName}.${collName}`;
    const timeField = "time";
    const metaField = "meta";
    const mongos = st.s;
    const db = mongos.getDB(dbName);

    assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
    assert.commandWorked(db.createCollection(
        collName,
        {timeseries: {timeField: timeField, metaField: metaField, granularity: 'seconds'}}));
    assert.commandWorked(mongos.adminCommand({
        shardCollection: viewNss,
        key: {[metaField]: 1},
    }));

    // Granularity updates works in 6.0
    assert.commandWorked(db.runCommand({collMod: collName, timeseries: {granularity: 'minutes'}}));

    // Granularity updates fails after downgrading.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: downgradeVersion}));
    assert.commandFailedWithCode(
        db.runCommand({collMod: collName, timeseries: {granularity: 'hours'}}),
        ErrorCodes.NotImplemented);

    st.stop();
}

runTest(lastContinuousFCV);
})();
