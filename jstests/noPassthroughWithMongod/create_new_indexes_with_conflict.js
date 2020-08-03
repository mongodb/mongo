/*
 * @tags: [
 *     # Uses failpoints that the mongos does not have.
 *     assumes_against_mongod_not_mongos,
 *     # Sets a failpoint on one mongod, so switching primaries would break the test.
 *     does_not_support_stepdowns,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");
load('jstests/libs/test_background_ops.js');

const dbName = "test";
const collName = "create_index_conflicts";
const testDB = db.getSiblingDB(dbName);
testDB.dropDatabase();
const testColl = testDB.getCollection(collName);

// Test scenario where a WriteConflictException is caught during createIndexes.
assert.commandWorked(db.adminCommand(
    {configureFailPoint: "createIndexesWriteConflict", mode: {activationProbability: 0.5}}));

// Make sure at least one execution occurs where the failpoint activates.
for (let i = 0; i < 20; i++) {
    const res = assert.commandWorked(testColl.createIndex({a: 1}));
    assert.eq(1, res.numIndexesBefore);
    assert.eq(2, res.numIndexesAfter);
    assert(res.createdCollectionAutomatically);
    testColl.drop();
}
assert.commandWorked(
    db.adminCommand({configureFailPoint: "createIndexesWriteConflict", mode: "off"}));

// Test scenario where a conflicting collection creation occurs, and createIndexes "loses" to the
// create collection.
assert.commandWorked(testDB.adminCommand({clearLog: 'global'}));

function runSuccessfulIndexBuild(dbName, collName) {
    jsTest.log("Index build request starting...");
    const res = db.getSiblingDB(dbName).runCommand(
        {createIndexes: collName, indexes: [{key: {b: 1}, name: "the_b_1_index"}]});
    jsTest.log("Index build request expected to succeed, result: " + tojson(res));
    assert.commandWorked(res);
    // Since the createCollection succeeded first, the index build should proceed as if the
    // collection already existed.
    assert(!res.createdCollectionAutomatically);
}

// Simulate a scenario where a createCollection succeeds before createIndexes can create the same
// collection.
function runConflictingCollectionCreate(testDB, dbName, collName) {
    testColl.drop();
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: 'hangBeforeCreateIndexesCollectionCreate', mode: 'alwaysOn'}));
    let joinCollectionCreate;
    let joinIndexBuild;
    try {
        jsTest.log("Starting a parallel shell to run index build request...");
        joinIndexBuild = startParallelShell(funWithArgs(runSuccessfulIndexBuild, dbName, collName),
                                            db.getMongo().port);
        jsTest.log("Waiting for create collection hang during index build...");
        checkLog.contains(
            db.getMongo(),
            "Hanging create collection due to failpoint 'hangBeforeCreateIndexesCollectionCreate'");

        jsTest.log("Create collection request starting...");
        const res = db.getSiblingDB(dbName).runCommand({create: collName});
        jsTest.log("Create collection request expected to succeed, result: " + tojson(res));
        assert.commandWorked(res);
    } finally {
        assert.commandWorked(testDB.adminCommand(
            {configureFailPoint: 'hangBeforeCreateIndexesCollectionCreate', mode: 'off'}));
    }

    joinIndexBuild();

    // Make sure the new index was successfully built, either by the parallel shell or via
    // createConflictingIndex. We should have the _id index and the 'the_b_1_index' index just built
    // in the parallel shell.
    assert.eq(testColl.getIndexes().length, 2);
}

runConflictingCollectionCreate(testDB, dbName, collName);
})();
