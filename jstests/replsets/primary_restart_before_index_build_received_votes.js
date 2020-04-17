/*
 * Tests that primary retains commit quorum provided to createIndexes across restarts.
 * @tags: [requires_persistence]
 */
(function() {

"use strict";
load("jstests/replsets/rslib.js");
load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const dbName = jsTest.name();
const collName = "coll";

let primary = rst.getPrimary();
let primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];
const collNss = primaryColl.getFullName();
const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(dbName);

if (!(IndexBuildTest.supportsTwoPhaseIndexBuild(primary) &&
      IndexBuildTest.indexBuildCommitQuorumEnabled(primary))) {
    jsTestLog(
        'Skipping test because two phase index build and index build commit quorum are not supported.');
    rst.stopSet();
    return;
}

jsTestLog("Do a document write.");
assert.commandWorked(
        primaryColl.insert({_id: 0, x: 0}, {"writeConcern": {"w": "majority"}}));
rst.awaitReplication();

// This makes sure the index build on primary hangs before receiving any votes from itself and
// secondary.
IndexBuildTest.pauseIndexBuilds(secondary);
IndexBuildTest.pauseIndexBuilds(primary);

jsTestLog("Start index build.");
const awaitBuild = IndexBuildTest.startIndexBuild(
    primary, collNss, {i: 1}, {}, [ErrorCodes.InterruptedDueToReplStateChange]);

jsTestLog("Wait for secondary to reach collection scan phase.");
IndexBuildTest.waitForIndexBuildToScanCollection(secondaryDB, collName, 'i_1');

jsTestLog("Wait for primary to reach collection scan phase.");
IndexBuildTest.waitForIndexBuildToScanCollection(primaryDB, collName, 'i_1');

jsTestLog("Restarting the primary");
rst.stop(primary, undefined, {skipValidation: true});
rst.start(primary, {}, true);

jsTestLog("Wait for primary to get re-elected.");
primary = rst.getPrimary();
primaryDB = primary.getDB(dbName);

awaitBuild();

jsTestLog("Resume index build on secondary.");
IndexBuildTest.resumeIndexBuilds(secondary);
IndexBuildTest.waitForIndexBuildToStop(primaryDB, collName, "i_1");
rst.awaitReplication();

// Check to see if the index was successfully created.
IndexBuildTest.assertIndexes(primaryDB[collName], 2, ['_id_', 'i_1']);

rst.stopSet();
})();
