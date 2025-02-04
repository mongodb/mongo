// Basic tests for $changeStream against all databases in the cluster.
// @tags: [
//   requires_profiling,
//   requires_fcv_81
// ]
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {
    assertInvalidChangeStreamNss,
    assertValidChangeStreamNss,
    ChangeStreamTest
} from "jstests/libs/query/change_stream_util.js";

const testDb = db.getSiblingDB(jsTestName());
const adminDB = testDb.getSiblingDB("admin");
const otherDB = testDb.getSiblingDB(jsTestName() + "_other");

// Drop and recreate the collections to be used in this set of tests.
assertDropAndRecreateCollection(testDb, "t1");
assertDropAndRecreateCollection(otherDB, "t2");

// Test that a change stream can be opened on the admin database if {allChangesForCluster:true}
// is specified.
assertValidChangeStreamNss("admin", 1, {allChangesForCluster: true});
// Test that a change stream cannot be opened on the admin database if a collection is
// specified, even with {allChangesForCluster:true}.
assertInvalidChangeStreamNss("admin", "testcoll", {allChangesForCluster: true});
// Test that a change stream cannot be opened on a database other than admin if
// {allChangesForCluster:true} is specified.
assertInvalidChangeStreamNss(testDb.getName(), 1, {allChangesForCluster: true});

let cst = new ChangeStreamTest(adminDB);
let cursor = cst.startWatchingAllChangesForCluster();

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

// Test that the change stream returns another inserted doc in a different database.
assert.commandWorked(otherDB.t2.insert({_id: 0, a: 2}));
expected = {
    documentKey: {_id: 0},
    fullDocument: {_id: 0, a: 2},
    ns: {db: otherDB.getName(), coll: "t2"},
    operationType: "insert",
};
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

// Test that the change stream returns an inserted doc on a user-created database whose name
// includes 'admin', 'local', or 'config'.
const validUserDBs = [
    "admin1",
    "1admin",
    "_admin_",
    "local_",
    "_local",
    "_local_",
    "config_",
    "_config",
    "_config_"
];
validUserDBs.forEach(dbName => {
    assert.commandWorked(testDb.getSiblingDB(dbName).test.insert({_id: 0, a: 1}));
    expected = [
        {
            documentKey: {_id: 0},
            fullDocument: {_id: 0, a: 1},
            ns: {db: dbName, coll: "test"},
            operationType: "insert",
        },
    ];
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});
});

// Test that the change stream returns an inserted doc on a user-created collection whose name
// includes "system" but is not considered an internal collection.
const validSystemColls = ["system", "systems.views", "ssystem.views", "test.system"];
validSystemColls.forEach(collName => {
    assert.commandWorked(testDb.getCollection(collName).insert({_id: 0, a: 1}));
    expected = [
        {
            documentKey: {_id: 0},
            fullDocument: {_id: 0, a: 1},
            ns: {db: testDb.getName(), coll: collName},
            operationType: "insert",
        },
    ];
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});
});

// Test that the change stream filters out operations on any collection in the 'admin', 'local',
// or 'config' databases.
const filteredDBs = ["admin", "local", "config"];
filteredDBs.forEach(dbName => {
    // Not allowed to use 'local' db through mongos.
    if (FixtureHelpers.isMongos(testDb) && dbName == "local")
        return;

    assert.commandWorked(testDb.getSiblingDB(dbName).test.insert({_id: 0, a: 1}));
    // Insert to the test collection to ensure that the change stream has something to
    // return.
    assert.commandWorked(testDb.t1.insert({_id: dbName}));
    expected = [
        {
            documentKey: {_id: dbName},
            fullDocument: {_id: dbName},
            ns: {db: testDb.getName(), coll: "t1"},
            operationType: "insert",
        },
    ];
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});
    // Clear the test collection to avoid duplicate key errors if this test is run multiple times.
    assert.commandWorked(testDb.getSiblingDB(dbName).test.remove({}, false /* justOne */));
});

// Dropping a database should generate drop entries for each collection followed by a database
// drop.
assert.commandWorked(otherDB.dropDatabase());
cst.assertDatabaseDrop({cursor: cursor, db: otherDB});

// Drop the remaining databases and clean up the test.
assert.commandWorked(testDb.dropDatabase());
validUserDBs.forEach(dbName => {
    testDb.getSiblingDB(dbName).dropDatabase();
});

// Test that getMore commands from the whole-cluster change stream are logged by the profiler.
if (!FixtureHelpers.isMongos(adminDB)) {
    assert.commandWorked(adminDB.runCommand({profile: 2}));
    cst.getNextBatch(cursor);
    const profileEntry = getLatestProfilerEntry(adminDB, {op: "getmore"});
    const firstStage = Object.keys(profileEntry.originatingCommand.pipeline[0])[0];
    assert(["$changeStream", "$_internalChangeStreamOplogMatch"].includes(firstStage),
           profileEntry);
}

cst.cleanUp();
