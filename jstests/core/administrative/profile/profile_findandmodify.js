// Confirms that profiled findAndModify execution contains all expected metrics with proper values.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   # Asserts on the number of index keys modified.
//   assumes_no_implicit_index_creation,
//   requires_fcv_70,
//   requires_profiling,
// ]

import {
    ClusteredCollectionUtil
} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {isLinux} from "jstests/libs/os_helpers.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

var testDB = db.getSiblingDB("profile_findandmodify");
assert.commandWorked(testDB.dropDatabase());
const collName = jsTestName();
var coll = testDB.getCollection(collName);

// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(testDB.setProfilingLevel(
    1, {filter: {'command.setFeatureCompatibilityVersion': {'$exists': false}}}));

//
// Update as findAndModify.
//
coll.drop();
for (var i = 0; i < 3; i++) {
    assert.commandWorked(coll.insert({_id: i, a: i, b: [0]}));
}
assert.commandWorked(coll.createIndex({b: 1}));

assert.eq({_id: 2, a: 2, b: [0]}, coll.findAndModify({
    query: {a: 2},
    update: {$inc: {"b.$[i]": 1}},
    collation: {locale: "fr"},
    arrayFilters: [{i: 0}]
}));

var profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.op, "command", tojson(profileObj));
assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
assert.eq(profileObj.command.query, {a: 2}, tojson(profileObj));
assert.eq(profileObj.command.update, {$inc: {"b.$[i]": 1}}, tojson(profileObj));
assert.eq(profileObj.command.collation, {locale: "fr"}, tojson(profileObj));
assert.eq(profileObj.command.arrayFilters, [{i: 0}], tojson(profileObj));
assert.eq(profileObj.keysExamined, 0, tojson(profileObj));
assert.eq(profileObj.docsExamined, 3, tojson(profileObj));
assert.eq(profileObj.nMatched, 1, tojson(profileObj));
assert.eq(profileObj.nModified, 1, tojson(profileObj));
assert.eq(profileObj.keysInserted, 1, tojson(profileObj));
assert.eq(profileObj.keysDeleted, 1, tojson(profileObj));
assert.eq(profileObj.planSummary, "COLLSCAN", tojson(profileObj));
assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));
if (isLinux()) {
    assert(profileObj.hasOwnProperty("cpuNanos"), tojson(profileObj));
}
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Delete as findAndModify.
//
coll.drop();
for (var i = 0; i < 3; i++) {
    assert.commandWorked(coll.insert({_id: i, a: i}));
}

const collectionIsClustered = ClusteredCollectionUtil.areAllCollectionsClustered(db.getMongo());
// A clustered collection doesn't actually have an index on _id.
const expectedKeysDeleted = collectionIsClustered ? 0 : 1;

assert.eq({_id: 2, a: 2}, coll.findAndModify({query: {a: 2}, remove: true}));
profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.op, "command", tojson(profileObj));
assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
assert.eq(profileObj.command.query, {a: 2}, tojson(profileObj));
assert.eq(profileObj.command.remove, true, tojson(profileObj));
assert.eq(profileObj.keysExamined, 0, tojson(profileObj));
assert.eq(profileObj.docsExamined, 3, tojson(profileObj));
assert.eq(profileObj.ndeleted, 1, tojson(profileObj));
assert.eq(profileObj.keysDeleted, expectedKeysDeleted, tojson(profileObj));
assert.eq(profileObj.planSummary, "COLLSCAN", tojson(profileObj));
assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Update with {upsert: true} as findAndModify.
//
coll.drop();
for (var i = 0; i < 3; i++) {
    assert.commandWorked(coll.insert({_id: i, a: i}));
}

// A clustered collection doesn't actually have an index on _id, and queries involve an efficient
// bounded collection scan.
const expectedKeysInserted = collectionIsClustered ? 0 : 1;
const expectedDocsExamined = 0;
assert.eq({_id: 4, a: 1},
          coll.findAndModify({query: {_id: 4}, update: {$inc: {a: 1}}, upsert: true, new: true}));
profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.op, "command", tojson(profileObj));
assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
assert.eq(profileObj.command.query, {_id: 4}, tojson(profileObj));
assert.eq(profileObj.command.update, {$inc: {a: 1}}, tojson(profileObj));
assert.eq(profileObj.command.upsert, true, tojson(profileObj));
assert.eq(profileObj.command.new, true, tojson(profileObj));
assert.eq(profileObj.keysExamined, 0, tojson(profileObj));
assert.eq(profileObj.docsExamined, expectedDocsExamined, tojson(profileObj));
assert.eq(profileObj.nMatched, 0, tojson(profileObj));
assert.eq(profileObj.nModified, 0, tojson(profileObj));
assert.eq(profileObj.nUpserted, 1, tojson(profileObj));
assert.eq(profileObj.keysInserted, expectedKeysInserted, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Idhack update as findAndModify.
//
coll.drop();
for (var i = 0; i < 3; i++) {
    assert.commandWorked(coll.insert({_id: i, a: i}));
}

const expectedKeysExamined = collectionIsClustered ? 0 : 1;
const expectedPlan = collectionIsClustered ? "CLUSTERED_IXSCAN" : "IDHACK";

assert.eq({_id: 2, a: 2}, coll.findAndModify({query: {_id: 2}, update: {$inc: {b: 1}}}));
profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.keysExamined, expectedKeysExamined, tojson(profileObj));
assert.eq(profileObj.docsExamined, 1, tojson(profileObj));
assert.eq(profileObj.nMatched, 1, tojson(profileObj));
assert.eq(profileObj.nModified, 1, tojson(profileObj));
assert.eq(profileObj.planSummary, expectedPlan, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Update as findAndModify with projection.
//
coll.drop();
for (var i = 0; i < 3; i++) {
    assert.commandWorked(coll.insert({_id: i, a: i}));
}

assert.eq({a: 2},
          coll.findAndModify({query: {a: 2}, update: {$inc: {b: 1}}, fields: {_id: 0, a: 1}}));
profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.op, "command", tojson(profileObj));
assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
assert.eq(profileObj.command.query, {a: 2}, tojson(profileObj));
assert.eq(profileObj.command.update, {$inc: {b: 1}}, tojson(profileObj));
assert.eq(profileObj.command.fields, {_id: 0, a: 1}, tojson(profileObj));
assert.eq(profileObj.keysExamined, 0, tojson(profileObj));
assert.eq(profileObj.docsExamined, 3, tojson(profileObj));
assert.eq(profileObj.nMatched, 1, tojson(profileObj));
assert.eq(profileObj.nModified, 1, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Delete as findAndModify with projection.
//
coll.drop();
for (var i = 0; i < 3; i++) {
    assert.commandWorked(coll.insert({_id: i, a: i}));
}

assert.eq({a: 2}, coll.findAndModify({query: {a: 2}, remove: true, fields: {_id: 0, a: 1}}));
profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.op, "command", tojson(profileObj));
assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
assert.eq(profileObj.command.query, {a: 2}, tojson(profileObj));
assert.eq(profileObj.command.remove, true, tojson(profileObj));
assert.eq(profileObj.command.fields, {_id: 0, a: 1}, tojson(profileObj));
assert.eq(profileObj.ndeleted, 1, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Confirm "hasSortStage" on findAndModify with sort.
//
coll.drop();
for (var i = 0; i < 3; i++) {
    assert.commandWorked(coll.insert({_id: i, a: i}));
}

assert.eq({_id: 0, a: 0},
          coll.findAndModify({query: {a: {$gte: 0}}, sort: {a: 1}, update: {$inc: {b: 1}}}));

profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.hasSortStage, true, tojson(profileObj));

//
// Confirm "fromMultiPlanner" metric.
//
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
for (i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}

coll.findAndModify({query: {a: 3, b: 3}, update: {$set: {c: 1}}});
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
