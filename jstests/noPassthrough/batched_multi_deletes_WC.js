/**
 * Validate basic batched multi-deletion handling with write conflicts.
 *
 * @tags: [
 *   requires_fcv_53,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For 'assertDropCollection()'
load("jstests/libs/profiler.js");                  // For 'getLatestProfilerEntry()'
load("jstests/libs/fail_point_util.js");           // For 'configureFailPoint()'

const conn = MongoRunner.runMongod();

// '__internalBatchedDeletesTesting.Collection0' is a special, hardcoded namespace that batches
// multi-doc deletes if the 'internalBatchUserMultiDeletesForTest' server parameter is set.
// TODO (SERVER-63044): remove this special handling.
const testDB = conn.getDB("__internalBatchedDeletesTesting");
const coll = testDB.getCollection("Collection0");
const collName = coll.getName();
const ns = coll.getFullName();

assertDropCollection(testDB, collName);

const collCount = 50002;  // Intentionally not a multiple of the default batch size.

assert.commandWorked(
    coll.insertMany([...Array(collCount).keys()].map(x => ({_id: x, a: "a".repeat(1024)}))));

assert.commandWorked(testDB.setProfilingLevel(2));

assert.commandWorked(
    testDB.adminCommand({setParameter: 1, internalBatchUserMultiDeletesForTest: 1}));

// While test is not deterministic, there will most likely be several write conflicts during the
// execution. ~250 write conflicts on average.
const batchedDeleteWriteConflictFP = configureFailPoint(
    testDB, "throwWriteConflictExceptionInBatchedDeleteStage", {}, {activationProbability: .005});

const commentID = jsTestName() + "_delete_op_id";
assert.commandWorked(testDB.runCommand({
    delete: collName,
    deletes: [{q: {_id: {$gte: 0}}, limit: 0}],
    comment: commentID,
    writeConcern: {w: "majority"}
}));
batchedDeleteWriteConflictFP.off();

// Confirm the metrics are as expected despite write conflicts.
const profileObj = getLatestProfilerEntry(testDB, {"command.comment": commentID});

assert(profileObj.writeConflicts);
jsTest.log(`The batched delete encountered ${profileObj.writeConflicts} writeConflicts`);

assert.eq(profileObj.execStats.stage, "BATCHED_DELETE");
assert.eq(collCount, profileObj.docsExamined);
assert.eq(collCount, profileObj.ndeleted);

assert.eq(0, coll.find().itcount());

MongoRunner.stopMongod(conn);
})();
