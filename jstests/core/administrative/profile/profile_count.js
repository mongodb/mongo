// Confirms that profiled count execution contains all expected metrics with proper values.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   does_not_support_stepdowns,
//   requires_fastcount,
//   requires_fcv_70,
//   requires_profiling,
//   # The test runs getLatestProfileEntry(). The downstream syncing node affects the profiler.
//   run_getLatestProfilerEntry,
// ]

import {isLinux} from "jstests/libs/os_helpers.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

let testDB = db.getSiblingDB("profile_count");
assert.commandWorked(testDB.dropDatabase());
let conn = testDB.getMongo();
const collName = jsTestName();
let coll = testDB.getCollection(collName);

// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(
    testDB.setProfilingLevel(1, {filter: {"command.setFeatureCompatibilityVersion": {"$exists": false}}}),
);

//
// Collection-level count.
//
let i;
for (i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}

assert.eq(10, coll.count({}, {collation: {locale: "fr"}}));

let profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
assert.eq(profileObj.op, "command", tojson(profileObj));
assert.eq(profileObj.protocol, "op_msg", tojson(profileObj));
assert.eq(profileObj.command.count, coll.getName(), tojson(profileObj));
assert.eq(profileObj.command.collation, {locale: "fr"}, tojson(profileObj));
assert.eq(profileObj.planSummary, "RECORD_STORE_FAST_COUNT", tojson(profileObj));
assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
assert(profileObj.hasOwnProperty("queryHash"), tojson(profileObj));
assert(profileObj.hasOwnProperty("planCacheKey"), tojson(profileObj));
assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));
if (isLinux()) {
    assert(profileObj.hasOwnProperty("cpuNanos"), tojson(profileObj));
}
assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Count with non-indexed query.
//
coll.drop();
for (i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}

let query = {a: {$gte: 5}};
assert.eq(5, coll.count(query));
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.command.query, query, tojson(profileObj));
assert.eq(profileObj.docsExamined, 10, tojson(profileObj));
assert(profileObj.hasOwnProperty("queryHash"), tojson(profileObj));
assert(profileObj.hasOwnProperty("planCacheKey"), tojson(profileObj));

//
// Count with indexed query.
//
coll.drop();
for (i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}
assert.commandWorked(coll.createIndex({a: 1}));

query = {
    a: {$gte: 5},
};
assert.eq(5, coll.count(query));
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.command.query, query, tojson(profileObj));
assert.eq(profileObj.keysExamined, 6, tojson(profileObj));
assert(profileObj.hasOwnProperty("queryHash"), tojson(profileObj));
assert(profileObj.hasOwnProperty("planCacheKey"), tojson(profileObj));
assert.eq(profileObj.planSummary, "COUNT_SCAN { a: 1 }", tojson(profileObj));
assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
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

assert.eq(1, coll.count({a: 3, b: 3}));
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));
