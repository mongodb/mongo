/**
 * Tests that a warning log message is emitted when a time series is sharded using a timeField as
 * part of the shard key.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1});

const dbName = 'test';
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
const timeseries = {
    timeField: 'time',
    metaField: 'hostId',
};

jsTestLog("Creating sharded time series");
assert.commandWorked(
    st.s.adminCommand({shardCollection: 'test.meta_only', key: {'hostId': 1}, timeseries}));
assert.soon(() => checkLog.checkContainsWithCountJson(st.rs0.getPrimary(), 8864700, {}, 0));

assert.commandWorked(
    st.s.adminCommand({shardCollection: 'test.time_only', key: {'time': 1}, timeseries}));
assert.soon(() => checkLog.checkContainsWithCountJson(st.rs0.getPrimary(), 8864700, {}, 1));

assert.commandWorked(st.s.adminCommand(
    {shardCollection: 'test.time_meta_compund', key: {'hostId': 1, 'time': 1}, timeseries}));
assert.soon(() => checkLog.checkContainsWithCountJson(st.rs0.getPrimary(), 8864700, {}, 2));

jsTestLog("Restarting sharded cluster");
st.stopAllConfigServers({}, true /* forRestart */);
st.restartAllConfigServers();

assert.soon(() => checkLog.checkContainsWithCountJson(st.configRS.getPrimary(), 8864701, {}, 2));

st.stop();
