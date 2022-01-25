/**
 * Test $collMod command on a sharded timeseries collection.
 *
 * @tags: [
 *   requires_fcv_51
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const dbName = 'testDB';
const collName = 'testColl';
const timeField = 'tm';
const metaField = 'mt';
const viewNss = `${dbName}.${collName}`;
const indexName = 'index';

function runTest(primaryDispatching) {
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

    // Setting collModPrimaryDispatching failpoint to make sure the fallback logic of dispatching
    // collMod command at primary shard works.
    if (primaryDispatching) {
        const primary = st.getPrimaryShard(dbName);
        assert.commandWorked(primary.adminCommand(
            {configureFailPoint: 'collModPrimaryDispatching', mode: 'alwaysOn'}));
    }

    // Granularity update works for unsharded time-series collection.
    assert.commandWorked(db.runCommand({collMod: collName, timeseries: {granularity: 'minutes'}}));

    // Shard the time-series collection.
    assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
    assert.commandWorked(db[collName].createIndex({[metaField]: 1}, {name: indexName}));
    assert.commandWorked(mongos.adminCommand({
        shardCollection: viewNss,
        key: {[metaField]: 1},
    }));

    // Normal collMod commands works for the sharded time-series collection.
    assert.commandWorked(
        db.runCommand({collMod: collName, index: {name: indexName, hidden: true}}));
    assert.commandWorked(
        db.runCommand({collMod: collName, index: {name: indexName, hidden: false}}));

    // Updates for timeField and metaField are disabled.
    assert.commandFailedWithCode(db.runCommand({collMod: collName, timeseries: {timeField: 'x'}}),
                                 40415 /* Failed to parse */);
    assert.commandFailedWithCode(db.runCommand({collMod: collName, timeseries: {metaField: 'x'}}),
                                 40415 /* Failed to parse */);
    // Granularity update is currently disabled for sharded time-series collection.
    assert.commandFailedWithCode(
        db.runCommand({collMod: collName, timeseries: {granularity: 'hours'}}),
        ErrorCodes.NotImplemented);

    st.stop();
}

runTest(false);
runTest(true);
})();
