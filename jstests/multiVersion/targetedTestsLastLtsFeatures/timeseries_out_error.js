/**
 * Tests that $out errors when trying to write to time-series collections on older server versions.
 * $out with the 'timeseries' option should only succeed if the FCV >= 7.1.
 */

(function() {
"use strict";

load('./jstests/multiVersion/libs/multi_cluster.js');  // for upgradeCluster.

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 2},
    mongos: 1,
    other: {
        mongosOptions: {binVersion: "last-lts"},
        configOptions: {binVersion: "last-lts"},
        shardOptions: {binVersion: "last-lts"},
        rsOptions: {binVersion: "last-lts"}
    }
});
st.configRS.awaitReplication();

const dbName = "test";
const testDB = st.s.getDB(dbName);
let coll = testDB["coll"];
let tColl = testDB["timeseries"];
coll.drop();
tColl.drop();

// set up a source collection and a time-series target collection.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
assert.commandWorked(coll.insert({t: ISODate(), m: 1}));
assert.commandWorked(testDB.createCollection(tColl.getName(), {timeseries: {timeField: "t"}}));
assert.commandWorked(tColl.insert({t: ISODate(), m: 1}));

// assert aggregate succeeds with no 'timeseries' option.
let pipeline = [{$out: "out"}];
assert.doesNotThrow(() => coll.aggregate(pipeline));
assert.eq(1, testDB["out"].find().itcount());

// assert aggregate fails with the original error with the 'timeseries' option.
pipeline = [{$out: {coll: "out_time", db: dbName, timeseries: {timeField: "t"}}}];
assert.throwsWithCode(() => coll.aggregate(pipeline), 16994);

// assert aggregate fails if trying to write to a time-series collection without the 'timeseries'
// option.
let replacePipeline = [{$out: tColl.getName()}];
assert.throwsWithCode(() => coll.aggregate(replacePipeline), ErrorCodes.InvalidOptions);

// upgrade the shards.
jsTestLog('upgrading the shards.');
st.upgradeCluster("latest", {upgradeMongos: false, upgradeConfigs: false});
awaitRSClientHosts(st.s, st.rs0.getPrimary(), {ok: true, ismaster: true});
// assert aggregate fails with the original error with the 'timeseries' option.
assert.throwsWithCode(() => coll.aggregate(pipeline), 16994);
// assert aggregate fails if trying to write to a time-series collection without the 'timeseries'
// option.
assert.throwsWithCode(() => coll.aggregate(replacePipeline), 7406100);

// upgrade the config server and mongos.
jsTestLog('upgrading the config server and mongos.');
st.upgradeCluster("latest", {upgradeShards: false, upgradeMongos: true, upgradeConfigs: true});
let mongosConn = st.s;
coll = mongosConn.getDB(dbName)["coll"];
// assert aggregate fails with an updated error with the 'timeseries' option.
assert.throwsWithCode(() => coll.aggregate(pipeline), 7406100);  // new error code.
// assert aggregate fails if trying to write to a time-series collection without the 'timeseries'
// option.
assert.throwsWithCode(() => coll.aggregate(replacePipeline), 7406100);

// upgrade the FCV version
jsTestLog('upgrading the FCV version.');
assert.commandWorked(mongosConn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
// assert aggregate with 'timeseries' succeeds.
assert.doesNotThrow(() => coll.aggregate(pipeline));
let resultColl = mongosConn.getDB(dbName)["out_time"];
assert.eq(1, resultColl.find().itcount());

// assert aggregate replacing a time-series collection without 'timeseries' succeeds.
assert.doesNotThrow(() => coll.aggregate(replacePipeline));
resultColl = mongosConn.getDB(dbName)["timeseries"];
assert.eq(1, resultColl.find().itcount());

st.stop();
}());
