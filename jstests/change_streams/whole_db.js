// Basic tests for $changeStream against all collections in a database.
// Do not run in whole-cluster passthrough since this test assumes that the change stream will be
// invalidated by a database drop.
// @tags: [
//   do_not_run_in_whole_cluster_passthrough,
//   requires_profiling,
//   requires_fcv_81
// ]
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {
    assertInvalidChangeStreamNss,
    ChangeStreamTest
} from "jstests/libs/query/change_stream_util.js";

const testDb = db.getSiblingDB(jsTestName());
assert.commandWorked(testDb.dropDatabase());

// Test that a single-database change stream cannot be opened on "admin", "config", or "local".
assertInvalidChangeStreamNss("admin", 1);
assertInvalidChangeStreamNss("config", 1);
if (!FixtureHelpers.isMongos(testDb)) {
    assertInvalidChangeStreamNss("local", 1);
}

let cst = new ChangeStreamTest(testDb);
let cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

// Test that if there are no changes, we return an empty batch.
assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

// Test that the change stream returns an inserted doc.
assert.commandWorked(testDb.t1.insert({_id: 0, a: 1}));
let expected = {
    documentKey: {_id: 0},
    fullDocument: {_id: 0, a: 1},
    ns: {db: testDb.getName(), coll: "t1"},
    operationType: "insert",
};
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

// Test that the change stream returns another inserted doc in a different collection but still
// in the target db.
assert.commandWorked(testDb.t2.insert({_id: 0, a: 2}));
expected = {
    documentKey: {_id: 0},
    fullDocument: {_id: 0, a: 2},
    ns: {db: testDb.getName(), coll: "t2"},
    operationType: "insert",
};
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

// Test that the change stream returns an inserted doc on a user-created collection whose name
// includes "system" but is not considered an internal collection.
const validSystemColls = ["system", "systems.views", "ssystem.views", "test.system"];
validSystemColls.forEach(collName => {
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});
    const coll = testDb.getCollection(collName);
    assert.commandWorked(coll.insert({_id: 0, a: 1}));
    expected = [
        {
            documentKey: {_id: 0},
            fullDocument: {_id: 0, a: 1},
            ns: {db: testDb.getName(), coll: collName},
            operationType: "insert",
        },
    ];
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});

    // Drop the collection and verify that the change stream picks up the "drop" notification.
    assertDropCollection(testDb, collName);
    // Insert to the test collection to queue up another change after the drop. This is needed
    // since the number of 'drop' notifications is not deterministic in the sharded passthrough
    // suites.
    assert.commandWorked(coll.insert({_id: 0}));
    cst.consumeDropUpTo({
        cursor: cursor,
        dropType: "drop",
        expectedNext: {
            documentKey: {_id: 0},
            fullDocument: {_id: 0},
            ns: {db: testDb.getName(), coll: collName},
            operationType: "insert",
        },
    });
});

// Test that getMore commands from the whole-db change stream are logged by the profiler.
if (!FixtureHelpers.isMongos(testDb)) {
    assert.commandWorked(testDb.runCommand({profile: 2}));
    cst.getNextBatch(cursor);
    const profileEntry = getLatestProfilerEntry(testDb, {op: "getmore"});
    const firstStage = Object.keys(profileEntry.originatingCommand.pipeline[0])[0];
    assert(["$changeStream", "$_internalChangeStreamOplogMatch"].includes(firstStage),
           profileEntry);
}

cst.cleanUp();
