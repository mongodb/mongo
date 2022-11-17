/**
 * Tests latest shards can process collMod on time-series collection sent from old 5.0 mongos.
 */

(function() {
"use strict";

const dbName = 'testDB';
const collName = 'testColl';
const timeField = 'tm';
const metaField = 'mt';
const indexName = 'index';
const viewNss = `${dbName}.${collName}`;

const st = new ShardingTest(
    {shards: 2, rs: {nodes: 3}, mongos: [{binVersion: 'latest'}, {binVersion: '5.0'}]});
const mongos = st.s0;
const db = mongos.getDB(dbName);

assert.commandWorked(
    mongos.adminCommand({setFeatureCompatibilityVersion: binVersionToFCV('latest')}));

assert.commandWorked(
    db.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}));
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
assert.commandWorked(db[collName].createIndex({[metaField]: 1}, {name: indexName}));
assert.commandWorked(mongos.adminCommand({
    shardCollection: viewNss,
    key: {[metaField]: 1},
}));

assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: '5.0'}));

const oldDb = st.s1.getDB(dbName);

assert.commandWorked(db[collName].createIndex({[timeField]: 1}));
// Assert that collMod works with matching versions of mongos and mongod.
assert.commandWorked(db.runCommand({collMod: collName, index: {name: 'tm_1', hidden: true}}));
// Assert that collMod still works with old version of mongos.
assert.commandWorked(oldDb.runCommand({collMod: collName, index: {name: 'tm_1', hidden: false}}));

// Assert that collMod with granularity update fails with matching versions of mongos and mongod.
assert.commandFailedWithCode(db.runCommand({collMod: collName, timeseries: {granularity: 'hours'}}),
                             ErrorCodes.NotImplemented);
// Assert that collMod with granularity update still fails with old version of mongos.
assert.commandFailedWithCode(
    oldDb.runCommand({collMod: collName, timeseries: {granularity: 'hours'}}),
    ErrorCodes.NotImplemented);

st.stop();
})();
