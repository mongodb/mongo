// Confirms that profiled distinct execution contains all expected metrics with proper values.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   does_not_support_stepdowns,
//   requires_fcv_70,
//   requires_profiling,
// ]

import {isLinux} from "jstests/libs/os_helpers.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

var testDB = db.getSiblingDB("profile_distinct");
assert.commandWorked(testDB.dropDatabase());
var conn = testDB.getMongo();
const collName = jsTestName();
var coll = testDB.getCollection(collName);

// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(testDB.setProfilingLevel(
    1, {filter: {'command.setFeatureCompatibilityVersion': {'$exists': false}}}));

//
// Confirm metrics for distinct with query.
//
var i;
for (i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i % 5, b: i}));
}
assert.commandWorked(coll.createIndex({b: 1}));

coll.distinct("a", {b: {$gte: 5}}, {collation: {locale: "fr"}});
var profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
assert.eq(profileObj.op, "command", tojson(profileObj));
assert.eq(profileObj.keysExamined, 5, tojson(profileObj));
assert.eq(profileObj.docsExamined, 5, tojson(profileObj));
assert.eq(profileObj.planSummary, "IXSCAN { b: 1 }", tojson(profileObj));
assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
assert.eq(profileObj.protocol, "op_msg", tojson(profileObj));
assert.eq(coll.getName(), profileObj.command.distinct, tojson(profileObj));
assert.eq(profileObj.command.collation, {locale: "fr"}, tojson(profileObj));
assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));
if (isLinux()) {
    assert(profileObj.hasOwnProperty("cpuNanos"), tojson(profileObj));
}
assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Confirm "fromMultiPlanner" metric.
//
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
for (i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}

coll.distinct("a", {a: 3, b: 3});
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));
