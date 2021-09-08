/**
 * Test $collMod command on a sharded timeseries collection.
 *
 * @tags: [
 *   requires_fcv_50
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

const mongo = new ShardingTest({shards: 2, rs: {nodes: 2}});
const mongos = mongo.s0;
const db = mongos.getDB(dbName);

if (!TimeseriesTest.timeseriesCollectionsEnabled(mongo.shard0)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    mongo.stop();
    return;
}

if (!TimeseriesTest.shardedtimeseriesCollectionsEnabled(mongo.shard0)) {
    jsTestLog("Skipping test because the sharded time-series collection feature flag is disabled");
    mongo.stop();
    return;
}

assert.commandWorked(
    db.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}));

// granularity update works for unsharded time-series colleciton.
assert.commandWorked(db.runCommand({collMod: collName, timeseries: {granularity: 'minutes'}}));

assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
assert.commandWorked(db[collName].createIndex({[metaField]: 1}));
assert.commandWorked(mongos.adminCommand({
    shardCollection: viewNss,
    key: {[metaField]: 1},
}));

// timeField and metaField updates are disabled.
assert.commandFailedWithCode(db.runCommand({collMod: collName, timeseries: {timeField: 'x'}}),
                             40415 /* Failed to parse */);
assert.commandFailedWithCode(db.runCommand({collMod: collName, timeseries: {metaField: 'x'}}),
                             40415 /* Failed to parse */);
// granularity update is currently disabled for sharded time-series collection.
assert.commandFailedWithCode(db.runCommand({collMod: collName, timeseries: {granularity: 'hours'}}),
                             ErrorCodes.NotImplemented);

mongo.stop();
})();
