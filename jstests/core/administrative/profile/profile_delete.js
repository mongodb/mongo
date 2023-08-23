// Confirms that profiled delete execution contains all expected metrics with proper values.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_security_token,
//   # Asserts on the number of index keys deleted.
//   assumes_no_implicit_index_creation,
//   does_not_support_stepdowns,
//   requires_fcv_70,
//   requires_non_retryable_writes,
//   requires_profiling,
//   # Uses $where operator
//   requires_scripting,
// ]

import {
    ClusteredCollectionUtil
} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {isLinux} from "jstests/libs/os_helpers.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

// Setup test db and collection.
var testDB = db.getSiblingDB("profile_delete");
assert.commandWorked(testDB.dropDatabase());
var coll = testDB.getCollection("test");

testDB.setProfilingLevel(2);

//
// Confirm metrics for single document delete.
//
var i;
for (i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}
assert.commandWorked(coll.createIndex({a: 1}));

assert.commandWorked(
    coll.remove({a: {$gte: 2}, b: {$gte: 2}}, {justOne: true, collation: {locale: "fr"}}));

var profileObj = getLatestProfilerEntry(testDB);

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
coll.drop();
for (i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}

assert.commandWorked(coll.remove({a: {$gte: 2}}));
profileObj = getLatestProfilerEntry(testDB);

// A clustered collection has no actual index on _id.
expectedKeysDeleted = collectionIsClustered ? 0 : 8;
assert.eq(profileObj.ndeleted, 8, tojson(profileObj));
assert.eq(profileObj.keysDeleted, expectedKeysDeleted, tojson(profileObj));
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

assert.commandWorked(coll.remove({a: 3, b: 3}));
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Confirm killing a remove operation will not log 'ndeleted' to the profiler.
//
assert(coll.drop());

for (let i = 0; i < 100; ++i) {
    assert.commandWorked(coll.insert({a: 1}));
}

const deleteResult = testDB.runCommand({
    delete: coll.getName(),
    deletes: [{q: {$where: "sleep(1000);return true", a: 1}, limit: 0}],
    maxTimeMS: 1
});

// This command will time out before completing.
assert.commandFailedWithCode(deleteResult, ErrorCodes.MaxTimeMSExpired);

profileObj = getLatestProfilerEntry(testDB);

// 'ndeleted' should not be defined.
assert(!profileObj.hasOwnProperty("ndeleted"), profileObj);
