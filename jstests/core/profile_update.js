// @tags: [
//   does_not_support_stepdowns,
//   requires_non_retryable_writes,
//   requires_profiling,
//   sbe_incompatible,
//   requires_fcv_47,
// ]

// Confirms that profiled update execution contains all expected metrics with proper values.

(function() {
"use strict";

load("jstests/libs/profiler.js");  // For getLatestProfilerEntry.

// Setup test db and collection.
var testDB = db.getSiblingDB("profile_update");
assert.commandWorked(testDB.dropDatabase());
var coll = testDB.getCollection("test");

testDB.setProfilingLevel(2);

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
// Confirm metrics for parameters that require "commands" mode.
//

if (db.getMongo().writeMode() === "commands") {
    coll.drop();
    assert.commandWorked(coll.insert({_id: 0, a: [0]}));

    assert.commandWorked(coll.update(
        {_id: 0}, {$set: {"a.$[i]": 1}}, {collation: {locale: "fr"}, arrayFilters: [{i: 0}]}));

    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "update", tojson(profileObj));
    assert.eq(profileObj.command.collation, {locale: "fr"}, tojson(profileObj));
    assert.eq(profileObj.command.arrayFilters, [{i: 0}], tojson(profileObj));
}

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

assert.eq(profileObj.command,
          {q: {_id: "new value", a: 4}, u: {$inc: {b: 1}}, multi: false, upsert: true},
          tojson(profileObj));
assert.eq(profileObj.keysExamined, 0, tojson(profileObj));
assert.eq(profileObj.docsExamined, 0, tojson(profileObj));
assert.eq(profileObj.keysInserted, 2, tojson(profileObj));
assert.eq(profileObj.nMatched, 0, tojson(profileObj));
assert.eq(profileObj.nModified, 0, tojson(profileObj));
assert.eq(profileObj.nUpserted, 1, tojson(profileObj));
assert.eq(profileObj.planSummary, "IXSCAN { _id: 1 }", tojson(profileObj));
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

    assert.eq(
        profileObj.command,
        {q: {_id: `new value ${index}`, a: index}, u: {$inc: {b: 1}}, multi: false, upsert: true},
        tojson(profileObj));
    assert.eq(profileObj.keysExamined, 0, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 0, tojson(profileObj));
    assert.eq(profileObj.keysInserted, 2, tojson(profileObj));
    assert.eq(profileObj.nMatched, 0, tojson(profileObj));
    assert.eq(profileObj.nModified, 0, tojson(profileObj));
    assert.eq(profileObj.nUpserted, 1, tojson(profileObj));
    assert.eq(profileObj.planSummary, "IXSCAN { _id: 1 }", tojson(profileObj));
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
})();
