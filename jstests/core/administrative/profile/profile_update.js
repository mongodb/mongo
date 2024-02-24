// Confirms that profiled update execution contains all expected metrics with proper values.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   # Asserts on the number of index keys modified.
//   assumes_no_implicit_index_creation,
//   does_not_support_stepdowns,
//   requires_non_retryable_writes,
//   requires_profiling,
// ]

import {
    ClusteredCollectionUtil
} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {getLatestProfilerEntry, getNLatestProfilerEntries} from "jstests/libs/profiler.js";

// Setup test db and collection.
var testDB = db.getSiblingDB("profile_update");
assert.commandWorked(testDB.dropDatabase());
const collName = jsTestName();
var coll = testDB.getCollection(collName);

// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(testDB.setProfilingLevel(
    1, {filter: {'command.setFeatureCompatibilityVersion': {'$exists': false}}}));

//
// Confirm metrics for single document update.
//
var i;
for (i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}
assert.commandWorked(coll.createIndex({a: 1}));

assert.commandWorked(coll.update({a: {$gte: 2}}, {$set: {c: 1}, $inc: {a: -10}}));

var profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
assert.eq(profileObj.op, "update", tojson(profileObj));
assert.eq(profileObj.keysExamined, 1, tojson(profileObj));
assert.eq(profileObj.docsExamined, 1, tojson(profileObj));
assert.eq(profileObj.keysInserted, 1, tojson(profileObj));
assert.eq(profileObj.keysDeleted, 1, tojson(profileObj));
assert.eq(profileObj.nMatched, 1, tojson(profileObj));
assert.eq(profileObj.nModified, 1, tojson(profileObj));
assert.eq(profileObj.planSummary, "IXSCAN { a: 1 }", tojson(profileObj));
assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Confirm metrics for parameters.
//
coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: [0]}));

assert.commandWorked(coll.update(
    {_id: 0}, {$set: {"a.$[i]": 1}}, {collation: {locale: "fr"}, arrayFilters: [{i: 0}]}));

profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
assert.eq(profileObj.op, "update", tojson(profileObj));
assert.eq(profileObj.command.collation, {locale: "fr"}, tojson(profileObj));
assert.eq(profileObj.command.arrayFilters, [{i: 0}], tojson(profileObj));

//
// Confirm metrics for multiple indexed document update.
//
coll.drop();
for (i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}
assert.commandWorked(coll.createIndex({a: 1}));

assert.commandWorked(coll.update({a: {$gte: 5}}, {$set: {c: 1}, $inc: {a: -10}}, {multi: true}));
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.keysExamined, 5, tojson(profileObj));
assert.eq(profileObj.docsExamined, 5, tojson(profileObj));
assert.eq(profileObj.keysInserted, 5, tojson(profileObj));
assert.eq(profileObj.keysDeleted, 5, tojson(profileObj));
assert.eq(profileObj.nMatched, 5, tojson(profileObj));
assert.eq(profileObj.nModified, 5, tojson(profileObj));
assert.eq(profileObj.planSummary, "IXSCAN { a: 1 }", tojson(profileObj));
assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Confirm metrics for insert on update with "upsert: true".
//
coll.drop();
for (i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}
assert.commandWorked(coll.createIndex({a: 1}));

assert.commandWorked(coll.update({_id: "new value", a: 4}, {$inc: {b: 1}}, {upsert: true}));
profileObj = getLatestProfilerEntry(testDB);

const collectionIsClustered = ClusteredCollectionUtil.areAllCollectionsClustered(db.getMongo());
const expectedPlan = collectionIsClustered ? "CLUSTERED_IXSCAN" : "IXSCAN { _id: 1 }";
const expectedKeysExamined = 0;
const expectedDocsExamined = collectionIsClustered ? 1 : 0;
const expectedKeysInserted = collectionIsClustered ? 1 : 2;

assert.eq(profileObj.command,
          {q: {_id: "new value", a: 4}, u: {$inc: {b: 1}}, multi: false, upsert: true},
          tojson(profileObj));
assert.eq(profileObj.keysExamined, expectedKeysExamined, tojson(profileObj));
assert.eq(profileObj.docsExamined, expectedDocsExamined, tojson(profileObj));
assert.eq(profileObj.keysInserted, expectedKeysInserted, tojson(profileObj));
assert.eq(profileObj.nMatched, 0, tojson(profileObj));
assert.eq(profileObj.nModified, 0, tojson(profileObj));
assert.eq(profileObj.nUpserted, 1, tojson(profileObj));
assert.eq(profileObj.planSummary, expectedPlan, tojson(profileObj));
assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Confirm metrics for batch insert on update with "upsert: true".
//
coll.drop();
for (i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}
assert.commandWorked(coll.createIndex({a: 1}));

assert.commandWorked(testDB.runCommand({
    update: coll.getName(),
    updates: [
        {q: {_id: "new value 4", a: 4}, u: {$inc: {b: 1}}, upsert: true},
        {q: {_id: "new value 5", a: 5}, u: {$inc: {b: 1}}, upsert: true},
        {q: {_id: "new value 6", a: 6}, u: {$inc: {b: 1}}, upsert: true}
    ],
    ordered: true
}));

// We need to check profiles for each individual update because they are logged separately.
const profiles = getNLatestProfilerEntries(testDB, 3);
assert.eq(profiles.length, 3, tojson(profiles));

const indices = [6, 5, 4];
for (var i = 0; i < indices.length; i++) {
    const profileObj = profiles[i];
    const index = indices[i];

    let expectedPlan = "IXSCAN { _id: 1 }";
    let expectedKeysExamined = 0;
    let expectedDocsExamined = 0;
    let expectedKeysInserted = 2;

    if (collectionIsClustered) {
        expectedPlan = "CLUSTERED_IXSCAN";
        expectedKeysExamined = 0;
        expectedDocsExamined = 1;
        expectedKeysInserted = 1;
    }

    assert.eq(
        profileObj.command,
        {q: {_id: `new value ${index}`, a: index}, u: {$inc: {b: 1}}, multi: false, upsert: true},
        tojson(profileObj));
    assert.eq(profileObj.keysExamined, expectedKeysExamined, tojson(profileObj));
    assert.eq(profileObj.docsExamined, expectedDocsExamined, tojson(profileObj));
    assert.eq(profileObj.keysInserted, expectedKeysInserted, tojson(profileObj));
    assert.eq(profileObj.nMatched, 0, tojson(profileObj));
    assert.eq(profileObj.nModified, 0, tojson(profileObj));
    assert.eq(profileObj.nUpserted, 1, tojson(profileObj));
    assert.eq(profileObj.planSummary, expectedPlan, tojson(profileObj));
    assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
    assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));
}

//
// Confirm "fromMultiPlanner" metric.
//
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
for (i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}

assert.commandWorked(coll.update({a: 3, b: 3}, {$set: {c: 1}}));
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));
