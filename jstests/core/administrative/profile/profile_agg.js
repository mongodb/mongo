// Confirms that profiled aggregation execution contains all expected metrics with proper values.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   does_not_support_stepdowns,
//   requires_fcv_70,
//   requires_profiling,
//   references_foreign_collection,
// ]

import {isLinux} from "jstests/libs/os_helpers.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

const testDB = db.getSiblingDB("profile_agg");
assert.commandWorked(testDB.dropDatabase());
const collName = jsTestName();
const coll = testDB.getCollection(collName);

// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(testDB.setProfilingLevel(
    1, {filter: {'command.setFeatureCompatibilityVersion': {'$exists': false}}}));

//
// Confirm metrics for agg w/ $match.
//
for (let i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}
assert.commandWorked(coll.createIndex({a: 1}));

assert.eq(8,
          coll.aggregate([{$match: {a: {$gte: 2}}}, {$sort: {b: 1}}, {$addFields: {c: 1}}],
                         {collation: {locale: "fr"}, comment: "agg_comment"})
              .itcount());
let profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
assert.eq(profileObj.op, "command", tojson(profileObj));
assert.eq(profileObj.nreturned, 8, tojson(profileObj));
assert.eq(profileObj.keysExamined, 8, tojson(profileObj));
assert.eq(profileObj.docsExamined, 8, tojson(profileObj));
assert.eq(profileObj.planSummary, "IXSCAN { a: 1 }", tojson(profileObj));
assert.eq(profileObj.protocol, "op_msg", tojson(profileObj));
assert.eq(profileObj.command.aggregate, coll.getName(), tojson(profileObj));
assert.eq(profileObj.command.collation, {locale: "fr"}, tojson(profileObj));
assert.eq(profileObj.command.comment, "agg_comment", tojson(profileObj));
assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));
if (isLinux()) {
    assert(profileObj.hasOwnProperty("cpuNanos"), tojson(profileObj));
}
assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));
assert(profileObj.hasOwnProperty("hasSortStage"), tojson(profileObj));
// Testing that 'usedDisk' is set when disk is used requires either using a lot of data or
// configuring a server parameter which could mess up other tests. This testing is
// done elsewhere so that this test can stay in the core suite
assert(!profileObj.hasOwnProperty("usedDisk"), tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

// Confirm that 'hasSortStage' is not present when the sort is non-blocking.
coll.aggregate([{$match: {a: {$gte: 2}}}, {$sort: {a: 1}}, {$addFields: {c: 1}}],
               {collation: {locale: "fr"}, comment: "agg_comment"});
profileObj = getLatestProfilerEntry(testDB);
assert(!profileObj.hasOwnProperty("hasSortStage"), tojson(profileObj));

//
// Confirm "fromMultiPlanner" metric.
//
assert(coll.drop());
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
for (let i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}

assert.eq(1, coll.aggregate([{$match: {a: 3, b: 3}}, {$addFields: {c: 1}}]).itcount());
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));

//
// Confirm that the correct namespace is written to the profiler when running an aggregation with a
// $out stage.
//
assert(coll.drop());
db.profile_agg_out.drop();
for (let i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}

assert.eq(0, coll.aggregate([{$match: {a: {$gt: 0}}}, {$out: "profile_agg_out"}]).itcount());
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));

//
// Confirm that the "hint" modifier is in the profiler document.
//
assert(coll.drop());
assert.commandWorked(coll.createIndex({a: 1}));
for (let i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}

assert.eq(
    1, coll.aggregate([{$match: {a: 3, b: 3}}, {$addFields: {c: 1}}], {hint: {_id: 1}}).itcount());
profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.command.hint, {_id: 1}, tojson(profileObj));

//
// Confirm that aggregations are truncated in the profiler as { $truncated: <string>, comment:
// <string> } when a comment parameter is provided.
//
let matchPredicate = {};

for (let i = 0; i < 501; i++) {
    matchPredicate[i] = "a".repeat(150);
}

assert.eq(coll.aggregate([{$match: matchPredicate}, {$addFields: {c: 1}}], {comment: "profile_agg"})
              .itcount(),
          0);
profileObj = getLatestProfilerEntry(testDB);
assert.eq((typeof profileObj.command.$truncated), "string", tojson(profileObj));
assert.eq(profileObj.command.comment, "profile_agg", tojson(profileObj));
