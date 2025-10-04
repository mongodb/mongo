// Confirms that profiled findAndModify execution contains all expected metrics with proper values.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: mapReduce,
//   # setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   requires_fcv_70,
//   requires_profiling,
//   uses_map_reduce_with_temp_collections,
//   # The test runs getLatestProfileEntry(). The downstream syncing node affects the profiler.
//   run_getLatestProfilerEntry,
// ]

import {isLinux} from "jstests/libs/os_helpers.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

const testDB = db.getSiblingDB("profile_mapreduce");
assert.commandWorked(testDB.dropDatabase());
const conn = testDB.getMongo();
const collName = jsTestName();
const coll = testDB.getCollection(collName);

// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(
    testDB.setProfilingLevel(1, {filter: {"command.setFeatureCompatibilityVersion": {"$exists": false}}}),
);

const mapFunction = function () {
    emit(this.a, this.b);
};

const reduceFunction = function (a, b) {
    return Array.sum(b);
};

//
// Confirm metrics for mapReduce with query.
//
for (let i = 0; i < 3; i++) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}
assert.commandWorked(coll.createIndex({a: 1}));

coll.mapReduce(mapFunction, reduceFunction, {query: {a: {$gte: 0}}, out: {inline: 1}, collation: {locale: "fr"}});

let profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
assert.eq(profileObj.op, "command", tojson(profileObj));
assert.eq(profileObj.keysExamined, 3, tojson(profileObj));
assert.eq(profileObj.docsExamined, 3, tojson(profileObj));
assert.eq(profileObj.planSummary, "IXSCAN { a: 1 }", tojson(profileObj));
assert.eq(profileObj.protocol, "op_msg", tojson(profileObj));
assert.eq(coll.getName(), profileObj.command.mapreduce, tojson(profileObj));
assert.eq({locale: "fr"}, profileObj.command.collation, tojson(profileObj));
assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));
if (isLinux()) {
    assert(profileObj.hasOwnProperty("cpuNanos"), tojson(profileObj));
}
assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Confirm metrics for mapReduce with sort stage.
//
assert(coll.drop());
for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}

coll.mapReduce(mapFunction, reduceFunction, {sort: {b: 1}, out: {inline: 1}});

profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.hasSortStage, true, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Confirm namespace field is correct when output is a collection.
//
assert(coll.drop());
for (let i = 0; i < 3; i++) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}

let outputCollectionName = "output_col";
coll.mapReduce(mapFunction, reduceFunction, {query: {a: {$gte: 0}}, out: outputCollectionName});

profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));

//
// Confirm "fromMultiPlanner" metric.
//
assert(coll.drop());
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
for (let i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}

coll.mapReduce(mapFunction, reduceFunction, {query: {a: 3, b: 3}, out: {inline: 1}});
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));
