// Confirms that profiled find execution contains all expected metrics with proper values.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   does_not_support_stepdowns,
//   requires_fcv_70,
//   requires_profiling,
// ]

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {isLinux} from "jstests/libs/os_helpers.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

var testDB = db.getSiblingDB("profile_find");
assert.commandWorked(testDB.dropDatabase());
const collName = jsTestName();
var coll = testDB.getCollection(collName);

// TODO SERVER-85238: Remove this check when replanning is properly implemented for classic runtime
// planning for SBE.
if (FeatureFlagUtil.isPresentAndEnabled(db, "ClassicRuntimePlanningForSbe")) {
    jsTestLog("Skipping test since featureFlagClassicRuntimePlanningForSbe is enabled");
    quit();
}

// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(testDB.setProfilingLevel(
    1, {filter: {'command.setFeatureCompatibilityVersion': {'$exists': false}}}));
const profileEntryFilter = {
    op: "query"
};

//
// Confirm most metrics on single document read.
//
var i;
for (i = 0; i < 3; ++i) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}
assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "fr"}}));

assert.eq(coll.find({a: 1}).collation({locale: "fr"}).limit(1).itcount(), 1);

var profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);

assert.eq(profileObj.ns, coll.getFullName(), profileObj);
assert.eq(profileObj.keysExamined, 1, profileObj);
assert.eq(profileObj.docsExamined, 1, profileObj);
assert.eq(profileObj.nreturned, 1, profileObj);
assert.eq(profileObj.planSummary, "IXSCAN { a: 1 }", profileObj);
assert(profileObj.execStats.hasOwnProperty("stage"), profileObj);
assert.eq(profileObj.command.filter, {a: 1}, profileObj);
assert.eq(profileObj.command.limit, 1, profileObj);
assert.eq(profileObj.protocol, "op_msg", profileObj);

assert.eq(profileObj.command.collation, {locale: "fr"});
assert.eq(profileObj.cursorExhausted, true, profileObj);
assert(!profileObj.hasOwnProperty("cursorid"), profileObj);
assert(profileObj.hasOwnProperty("responseLength"), profileObj);
if (isLinux()) {
    assert(profileObj.hasOwnProperty("cpuNanos"), tojson(profileObj));
}
assert(profileObj.hasOwnProperty("millis"), profileObj);
assert(profileObj.hasOwnProperty("numYield"), profileObj);
assert(profileObj.hasOwnProperty("locks"), profileObj);
assert(profileObj.locks.hasOwnProperty("Global"), profileObj);
assert.eq(profileObj.appName, "MongoDB Shell", profileObj);

//
// Confirm "cursorId" and "hasSortStage" metrics.
//
coll.drop();
for (i = 0; i < 3; ++i) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}
assert.commandWorked(coll.createIndex({a: 1}));

assert.neq(coll.findOne({a: 1}), null);

assert.neq(coll.find({a: {$gte: 0}}).sort({b: 1}).batchSize(1).next(), null);
profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);

assert.eq(profileObj.hasSortStage, true, profileObj);
assert(profileObj.hasOwnProperty("cursorid"), profileObj);
assert(!profileObj.hasOwnProperty("cursorExhausted"), profileObj);
assert.eq(profileObj.appName, "MongoDB Shell", profileObj);

//
// Confirm "fromMultiPlanner" metric.
//
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
for (i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}

assert.neq(coll.findOne({}), null);
profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
assert.eq(profileObj.fromMultiPlanner, undefined, profileObj);
assert.eq(profileObj.appName, "MongoDB Shell", profileObj);

assert.neq(coll.findOne({a: 3, b: 3}), null);
profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
assert.eq(profileObj.fromMultiPlanner, true, profileObj);
assert.eq(profileObj.appName, "MongoDB Shell", profileObj);

//
// Confirm "replanned" metric.
// We should ideally be using a fail-point to trigger "replanned" rather than relying on
// current query planner behavior knowledge to setup a scenario. SERVER-23620 has been entered
// to add this fail-point and to update appropriate tests.
//
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
for (i = 0; i < 20; ++i) {
    assert.commandWorked(coll.insert({a: 5, b: i}));
    assert.commandWorked(coll.insert({a: i, b: 10}));
}

// Until we get the failpoint described in the above comment (regarding SERVER-23620), we must
// run the query twice. The first time will create an inactive cache entry. The second run will
// take the same number of works, and create an active cache entry.
assert.neq(coll.findOne({a: 5, b: 15}), null);
assert.neq(coll.findOne({a: 5, b: 15}), null);

// Run a query with the same shape, but with different parameters. The plan cached for the
// query above will perform poorly (since the selectivities are different) and we will be
// forced to replan.
assert.neq(coll.findOne({a: 15, b: 10}), null);
profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);

assert.eq(profileObj.replanned, true, profileObj);
assert(profileObj.hasOwnProperty('replanReason'), profileObj);
assert(
    profileObj.replanReason.match(
        /cached plan was less efficient than expected: expected trial execution to take [0-9]+ (works|reads) but it took at least [0-9]+ (works|reads)/),
    profileObj);
assert.eq(profileObj.appName, "MongoDB Shell", profileObj);

//
// Confirm that query modifiers such as "hint" are in the profiler document.
//
coll.drop();
assert.commandWorked(coll.insert({_id: 2}));

assert.eq(coll.find().hint({_id: 1}).itcount(), 1);
profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
assert.eq(profileObj.command.hint, {_id: 1}, profileObj);

assert.eq(coll.find().comment("a comment").itcount(), 1);
profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
assert.eq(profileObj.command.comment, "a comment", profileObj);

var maxTimeMS = 100000;
assert.eq(coll.find().maxTimeMS(maxTimeMS).itcount(), 1);
profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
assert.eq(profileObj.command.maxTimeMS, maxTimeMS, profileObj);

assert.eq(coll.find().max({_id: 3}).hint({_id: 1}).itcount(), 1);
profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
assert.eq(profileObj.command.max, {_id: 3}, profileObj);

assert.eq(coll.find().min({_id: 0}).hint({_id: 1}).itcount(), 1);
profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
assert.eq(profileObj.command.min, {_id: 0}, profileObj);

assert.eq(coll.find().returnKey().itcount(), 1);
profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
assert.eq(profileObj.command.returnKey, true, profileObj);

//
// Confirm that queries are truncated in the profiler as { $truncated: <string>, comment:
// <string> }
//
let queryPredicate = {};

for (let i = 0; i < 501; i++) {
    queryPredicate[i] = "a".repeat(150);
}

assert.eq(coll.find(queryPredicate).comment("profile_find").itcount(), 0);
profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
assert.eq((typeof profileObj.command.$truncated), "string", profileObj);
assert.eq(profileObj.command.comment, "profile_find", profileObj);

//
// Confirm that a query whose filter contains a field named 'query' appears as expected in the
// profiler. This test ensures that upconverting a legacy query correctly identifies this as a
// user field rather than a wrapped filter spec.
//
coll.find({query: "foo"}).itcount();
profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
assert.eq(profileObj.command.filter, {query: "foo"}, profileObj);
