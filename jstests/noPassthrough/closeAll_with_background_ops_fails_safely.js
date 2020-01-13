/**
 * SERVER-35671: Ensure that if database has background operations it can't be closed and that
 * attempting to close it won't leave it in an inconsistant state.
 *
 * @tags: [requires_replication, uses_transactions]
 */

(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

let replSet = new ReplSetTest({name: "server35671", nodes: 1});
let setFailpointBool = (testDB, failpointName, alwaysOn, times) => {
    if (times) {
        return testDB.adminCommand({configureFailPoint: failpointName, mode: {"times": times}});
    } else if (alwaysOn) {
        return testDB.adminCommand({configureFailPoint: failpointName, mode: "alwaysOn"});
    } else {
        return testDB.adminCommand({configureFailPoint: failpointName, mode: "off"});
    }
};
replSet.startSet();
replSet.initiate();
let testDB = replSet.getPrimary().getDB("test");
// This test depends on using the IndexBuildsCoordinator to build this index, which as of
// SERVER-44405, will not occur in this test unless the collection is created beforehand.
assert.commandWorked(testDB.runCommand({create: "coll"}));

// Insert document into collection to avoid optimization for index creation on an empty collection.
// This allows us to pause index builds on the collection using a fail point.
assert.commandWorked(testDB.getCollection("coll").insert({a: 1}));

setFailpointBool(testDB, "hangAfterStartingIndexBuildUnlocked", true);

// Blocks because of failpoint
let join = startParallelShell(
    "db.getSiblingDB('test').coll.createIndex({a: 1, b: 1}, {background: true})", replSet.ports[0]);

// Let the createIndex start to run.
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, "coll", "a_1_b_1");

// Repeated calls should continue to fail without crashing.
assert.commandFailed(testDB.adminCommand({restartCatalog: 1}));
assert.commandFailed(testDB.adminCommand({restartCatalog: 1}));
assert.commandFailed(testDB.adminCommand({restartCatalog: 1}));

// Unset failpoint so we can join the parallel shell.
setFailpointBool(testDB, "hangAfterStartingIndexBuildUnlocked", false);
join();
replSet.stopSet();
})();
