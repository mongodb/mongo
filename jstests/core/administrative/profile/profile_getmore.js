// Confirms that profiled getMore execution contains all expected metrics with proper values.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_security_token,
//   does_not_support_stepdowns,
//   requires_getmore,
//   requires_fcv_70,
//   requires_profiling,
// ]

import {isLinux} from "jstests/libs/os_helpers.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

const testDB = db.getSiblingDB("profile_getmore");
assert.commandWorked(testDB.dropDatabase());
const coll = testDB.getCollection("test");

testDB.setProfilingLevel(2);

let i;
//
// Confirm basic metrics on getMore with a not-exhausted cursor.
//
for (i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}
assert.commandWorked(coll.createIndex({a: 1}));

let cursor = coll.find({a: {$gt: 0}}).sort({a: 1}).batchSize(2);
cursor.next();  // Perform initial query and consume first of 2 docs returned.

let cursorId = getLatestProfilerEntry(testDB, {op: "query"}).cursorid;  // Save cursorid from find.

cursor.next();  // Consume second of 2 docs from initial query.
cursor.next();  // getMore performed, leaving open cursor.

let profileObj = getLatestProfilerEntry(testDB, {op: "getmore"});

assert.eq(profileObj.ns, coll.getFullName(), profileObj);
assert.eq(profileObj.op, "getmore", profileObj);
assert.eq(profileObj.keysExamined, 2, profileObj);
assert.eq(profileObj.docsExamined, 2, profileObj);
assert.eq(profileObj.cursorid, cursorId, profileObj);
assert.eq(profileObj.nreturned, 2, profileObj);
assert.eq(profileObj.command.getMore, cursorId, profileObj);
assert.eq(profileObj.command.collection, coll.getName(), profileObj);
assert.eq(profileObj.command.batchSize, 2, profileObj);
assert.eq(profileObj.originatingCommand.filter, {a: {$gt: 0}});
assert.eq(profileObj.originatingCommand.sort, {a: 1});
assert.eq(profileObj.planSummary, "IXSCAN { a: 1 }", profileObj);
assert(profileObj.hasOwnProperty("execStats"), profileObj);
assert(profileObj.execStats.hasOwnProperty("stage"), profileObj);
assert(profileObj.hasOwnProperty("responseLength"), profileObj);
if (isLinux()) {
    assert(profileObj.hasOwnProperty("cpuNanos"), tojson(profileObj));
}
assert(profileObj.hasOwnProperty("numYield"), profileObj);
assert(profileObj.hasOwnProperty("locks"), profileObj);
assert(profileObj.locks.hasOwnProperty("Global"), profileObj);
assert(profileObj.hasOwnProperty("millis"), profileObj);
assert(!profileObj.hasOwnProperty("cursorExhausted"), profileObj);
assert.eq(profileObj.appName, "MongoDB Shell", profileObj);
assert(profileObj.hasOwnProperty("queryHash"), profileObj);

//
// Confirm hasSortStage on getMore with a not-exhausted cursor and in-memory sort.
//
coll.drop();
for (i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}

cursor = coll.find({a: {$gt: 0}}).sort({a: 1}).batchSize(2);
cursor.next();  // Perform initial query and consume first of 2 docs returned.
cursor.next();  // Consume second of 2 docs from initial query.
cursor.next();  // getMore performed, leaving open cursor.

profileObj = getLatestProfilerEntry(testDB, {op: "getmore"});

assert.eq(profileObj.hasSortStage, true, profileObj);

//
// Confirm "cursorExhausted" metric.
//
coll.drop();
for (i = 0; i < 3; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}

cursor = coll.find().batchSize(2);
cursor.next();     // Perform initial query and consume first of 3 docs returned.
cursor.itcount();  // Exhaust the cursor.

profileObj = getLatestProfilerEntry(testDB, {op: "getmore"});

assert(profileObj.hasOwnProperty("cursorid"),
       profileObj);  // cursorid should always be present on getMore.
assert.neq(0, profileObj.cursorid, profileObj);
assert.eq(profileObj.cursorExhausted, true, profileObj);
assert.eq(profileObj.appName, "MongoDB Shell", profileObj);

//
// Confirm getMore on aggregation.
//
coll.drop();
for (i = 0; i < 20; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}
assert.commandWorked(coll.createIndex({a: 1}));

cursor = coll.aggregate([{$match: {a: {$gte: 0}}}], {cursor: {batchSize: 0}, hint: {a: 1}});
cursorId = getLatestProfilerEntry(testDB, {"command.aggregate": coll.getName()}).cursorid;
assert.neq(0, cursorId);

cursor.next();  // Consume the result set.

profileObj = getLatestProfilerEntry(testDB, {op: "getmore"});

assert.eq(profileObj.ns, coll.getFullName(), profileObj);
assert.eq(profileObj.op, "getmore", profileObj);
assert.eq(profileObj.command.getMore, cursorId, profileObj);
assert.eq(profileObj.command.collection, coll.getName(), profileObj);
assert.eq(profileObj.originatingCommand.pipeline[0], {$match: {a: {$gte: 0}}}, profileObj);
assert.eq(profileObj.cursorid, cursorId, profileObj);
assert.eq(profileObj.nreturned, 20, profileObj);
assert.eq(profileObj.planSummary, "IXSCAN { a: 1 }", profileObj);
assert.eq(profileObj.cursorExhausted, true, profileObj);
assert.eq(profileObj.keysExamined, 20, profileObj);
assert.eq(profileObj.docsExamined, 20, profileObj);
assert.eq(profileObj.appName, "MongoDB Shell", profileObj);
assert.eq(profileObj.originatingCommand.hint, {a: 1}, profileObj);

//
// Confirm that originatingCommand is truncated in the profiler as { $truncated: <string>,
// comment: <string> }
//
let docToInsert = {};

for (i = 0; i < 501; i++) {
    docToInsert[i] = "a".repeat(150);
}

coll.drop();
for (i = 0; i < 4; i++) {
    assert.commandWorked(coll.insert(docToInsert));
}

cursor = coll.find(docToInsert).comment("profile_getmore").batchSize(2);
assert.eq(cursor.itcount(), 4);  // Consume result set and trigger getMore.

profileObj = getLatestProfilerEntry(testDB, {op: "getmore"});
assert.eq((typeof profileObj.originatingCommand.$truncated), "string", profileObj);
assert.eq(profileObj.originatingCommand.comment, "profile_getmore", profileObj);
