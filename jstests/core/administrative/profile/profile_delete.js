// Confirms that profiled delete execution contains all expected metrics with proper values.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   # Asserts on the number of index keys deleted.
//   assumes_no_implicit_index_creation,
//   does_not_support_stepdowns,
//   requires_fcv_70,
//   requires_non_retryable_writes,
//   requires_profiling,
//   # Uses $where operator
//   requires_scripting,
//   # TODO SERVER-89016 Remove this tag
//   does_not_support_multiplanning_single_solutions,
//   # The test runs getLatestProfileEntry(). The downstream syncing node affects the profiler.
//   run_getLatestProfilerEntry,
// ]

import {
    ClusteredCollectionUtil
} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {isLinux} from "jstests/libs/os_helpers.js";
import {
    getLatestProfilerEntry,
    profilerHasZeroMatchingEntriesOrThrow
} from "jstests/libs/profiler.js";

// Setup test db and collection.
const testDB = db.getSiblingDB("profile_delete");
assert.commandWorked(testDB.dropDatabase());
const collName = jsTestName();
const coll = testDB.getCollection(collName);

// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(testDB.setProfilingLevel(
    1, {filter: {'command.setFeatureCompatibilityVersion': {'$exists': false}}}));

//
// Confirm metrics for single document delete.
//
let docs = [];
for (let i = 0; i < 10; ++i) {
    docs.push({a: i, b: i});
}
assert.commandWorked(coll.insert(docs));
assert.commandWorked(coll.createIndex({a: 1}));

let testComment = "test1";
assert.commandWorked(testDB.runCommand({
    delete: collName,
    deletes: [{q: {a: {$gte: 2}, b: {$gte: 2}}, limit: 1, collation: {locale: "fr"}}],
    comment: testComment
}));

let profileObj = getLatestProfilerEntry(testDB, {"command.comment": testComment});

const collectionIsClustered = ClusteredCollectionUtil.areAllCollectionsClustered(db.getMongo());
// A clustered collection has no actual index on _id.
let expectedKeysDeleted = collectionIsClustered ? 1 : 2;

assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
assert.eq(profileObj.op, "remove", tojson(profileObj));
assert.eq(profileObj.command.collation, {locale: "fr"}, tojson(profileObj));
assert.eq(profileObj.ndeleted, 1, tojson(profileObj));
assert.eq(profileObj.keysExamined, 1, tojson(profileObj));
assert.eq(profileObj.docsExamined, 1, tojson(profileObj));
assert.eq(profileObj.keysDeleted, expectedKeysDeleted, tojson(profileObj));
assert.eq(profileObj.planSummary, "IXSCAN { a: 1 }", tojson(profileObj));
assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
if (isLinux()) {
    assert(profileObj.hasOwnProperty("cpuNanos"), tojson(profileObj));
}
assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Confirm metrics for multiple document delete.
//
assert(coll.drop());
docs = [];
for (let i = 0; i < 10; ++i) {
    docs.push({a: i});
}
assert.commandWorked(coll.insert(docs));

testComment = "test2";
assert.commandWorked(testDB.runCommand(
    {delete: collName, deletes: [{q: {a: {$gte: 2}}, limit: 0}], comment: testComment}));

profileObj = getLatestProfilerEntry(testDB, {"command.comment": testComment});

// A clustered collection has no actual index on _id.
expectedKeysDeleted = collectionIsClustered ? 0 : 8;
assert.eq(profileObj.ndeleted, 8, tojson(profileObj));
assert.eq(profileObj.keysDeleted, expectedKeysDeleted, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Confirm "fromMultiPlanner" metric.
//
assert(coll.drop());
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
docs = [];
for (let i = 0; i < 5; ++i) {
    docs.push({a: i, b: i});
}
assert.commandWorked(coll.insert(docs));

testComment = "test3";
assert.commandWorked(testDB.runCommand(
    {delete: collName, deletes: [{q: {a: 3, b: 3}, limit: 0}], comment: testComment}));

profileObj = getLatestProfilerEntry(testDB, {"command.comment": testComment});

assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Confirm killing a remove operation will not log 'ndeleted' to the profiler.
//
assert(coll.drop());

docs = [];
for (let i = 0; i < 100; ++i) {
    docs.push({a: 1});
}
assert.commandWorked(coll.insert(docs));

testComment = "test4";
const deleteResult = testDB.runCommand({
    delete: coll.getName(),
    deletes: [{q: {$where: "sleep(1000);return true", a: 1}, limit: 0}],
    maxTimeMS: 1,
    comment: testComment
});

// This command will time out before completing.
assert.commandFailedWithCode(deleteResult, ErrorCodes.MaxTimeMSExpired);

// Depending on where in the command path the maxTimeMS timeout occurs we will either forego writing
// a document to the system.profile collection or we will write the delete command entry that
// reflects failed command execution.
profilerHasZeroMatchingEntriesOrThrow(
    {profileDB: testDB, filter: {"command.comment": testComment, "ok": 1}});
