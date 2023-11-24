/**
 * Tests that the query rewrites for fixed bucketing can successfully be handled by a mixed cluster.
 * The primary shard rewrites the query and sends it to the secondary shard that can handle running
 * the rewritten query, even though the secondary shard's FCV is downgraded to 7.0.
 */
import "jstests/multiVersion/libs/multi_cluster.js";

import {getAggPlanStages} from "jstests/libs/analyze_plan.js";
import {awaitRSClientHosts} from "jstests/replsets/rslib.js";

(function() {
"use strict";

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 2},
    mongos: 1,
    other: {
        mongosOptions: {binVersion: "latest"},
        configOptions: {binVersion: "latest"},
        shardOptions: {binVersion: "latest"},
        rsOptions: {binVersion: "latest"}
    }
});
st.configRS.awaitReplication();

// Create a sharded time-series collection that has fixed buckets.
const dbName = "test";
let testDB = st.s.getDB(dbName);
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

let coll = testDB["timeseries"];
const timeField = "t";
coll.drop();
const shardKey = {
    [timeField]: 1
};

function runPipeline(predValue) {
    // Run the query and confirm the correct results were returned.
    let predicate = ISODate("2022-11-12");
    if (predValue) {
        predicate = predValue;
    }
    const pipeline = [{$match: {[timeField]: {$lt: predicate}}}];
    const results = coll.aggregate(pipeline).toArray();
    assert.sameMembers(results, [docs[0], docs[1], docs[2]]);

    const explain = coll.explain().aggregate(pipeline);
    const unpackStage = getAggPlanStages(explain, "$_internalUnpackBucket");
    // If data is on both of the shards, we will see 2 "$_internalUnpackBucket" stages in the
    // explain plan.
    assert.gte(unpackStage.length, 1, `Expected $_internalUnpackBucket in ${tojson(explain)}`);
    return unpackStage[0]["$_internalUnpackBucket"];
}

assert.commandWorked(testDB.adminCommand({
    shardCollection: `${dbName}.${coll.getName()}`,
    key: shardKey,
    timeseries: {timeField, bucketRoundingSeconds: 3600, bucketMaxSpanSeconds: 3600}
}));

// Set up the shards such that the primary shard has [MinKey, 2022-09-30), and the other shard has
// [2022-09-30, MaxKey].
let splitPoint = {[`control.min.${timeField}`]: ISODate(`2022-09-30`)};
assert.commandWorked(
    testDB.adminCommand({split: `${dbName}.system.buckets.${coll.getName()}`, middle: splitPoint}));

// Move one of the chunks into the second shard.
const primaryShard = st.getPrimaryShard(dbName);
const otherShard = st.getOther(primaryShard);
assert.commandWorked(testDB.adminCommand({
    movechunk: `${dbName}.system.buckets.${coll.getName()}`,
    find: splitPoint,
    to: otherShard.name,
    _waitForDelete: true
}));

// Insert 4 documents and expect 2 documents on each shard.
const docs = [
    {_id: 0, [timeField]: ISODate("2017-10-12")},
    {_id: 1, [timeField]: ISODate("2018-10-12")},
    {_id: 2, [timeField]: ISODate("2022-10-12")},
    {_id: 3, [timeField]: ISODate("2023-11-12")},
];
coll.insertMany(docs);

// Run the pipeline, and expect no eventFilter, since the rewrite should occur.
let unpackStage = runPipeline();
assert(!unpackStage["eventFilter"], "Expected no eventFilter, but got: " + tojson(unpackStage));

// Run a different pipeline, and expect an eventFilter. The rewrite does not apply because the
// predicate passed in does not align with the bucket boundaries.
unpackStage = runPipeline(ISODate("2022-11-12T07:30:10.957Z") /* predValue */);
assert(unpackStage["eventFilter"], "Expected an eventFilter, but got: " + tojson(unpackStage));

// Downgrade the FCV version.
jsTestLog('downgrading the FCV version.');
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: '7.0', confirm: true}));

// Run the pipeline, and expect an eventFilter, since the FCV version is downgraded.
unpackStage = runPipeline();
assert(unpackStage["eventFilter"], "Expected an eventFilter, but got: " + tojson(unpackStage));

// Downgrade the mongos binary.
jsTestLog('downgrading mongos.');
st.restartBinariesWithDowngradeBackCompat('latest');
st.downgradeBinariesWithoutDowngradeBackCompat('last-lts',
                                               {downgradeShards: false, downgradeConfigs: false});
let mongosConn = st.s;
testDB = mongosConn.getDB(dbName);
coll = testDB[coll.getName()];

// Downgrade the other shard's binary. We now have a mixed cluster.
jsTestLog('downgrading other shard.');
st.downgradeBinariesWithoutDowngradeBackCompat('last-lts', {
    downgradeShards: false,
    downgradeOneShard: st.rs1,
    downgradeMongos: false,
    downgradeConfigs: false
});
awaitRSClientHosts(st.s, st.rs0.getPrimary(), {ok: true, ismaster: true});
awaitRSClientHosts(st.s, st.rs1.getPrimary(), {ok: true, ismaster: true});

// Before running the query, restart the profiler.
const primaryDB = st.shard0.getDB(dbName);
const otherDB = st.shard1.getDB(dbName);
for (let shardDB of [primaryDB, otherDB]) {
    shardDB.setProfilingLevel(0);
    shardDB.system.profile.drop();
    shardDB.setProfilingLevel(2);
}
// Run the pipeline, and expect an eventFilter, since the FCV version is still downgraded.
unpackStage = runPipeline();
assert(unpackStage["eventFilter"], "Expected an eventFilter, but got: " + tojson(unpackStage));

// Confirm the other shard ran the query.
let filter = {"command.aggregate": `system.buckets.${coll.getName()}`};
const otherShardEntries = otherDB.system.profile.find(filter).toArray();
assert.eq(otherShardEntries.length, 1, otherShardEntries);

// Downgrade the rest of the shards and the config server.
jsTestLog('downgrading the rest of the shards and the config server.');
st.downgradeBinariesWithoutDowngradeBackCompat('last-lts', {downgradeMongos: false});
awaitRSClientHosts(st.s, st.rs0.getPrimary(), {ok: true, ismaster: true});
awaitRSClientHosts(st.s, st.rs1.getPrimary(), {ok: true, ismaster: true});

// Run the pipeline, and expect an eventFilter, since the FCV version is still downgraded.
unpackStage = runPipeline();
assert(unpackStage["eventFilter"], "Expected an eventFilter, but got: " + tojson(unpackStage));

st.stop();
}());
