/*
 * Test that primary rollbacks before receiving any votes from the replica set should not
 * make createIndexes command's commit quorum value to be lost.
 * @tags: [
 *   # TODO(SERVER-107055): Primary-driven index builds don't support failover yet.
 *   primary_driven_index_builds_incompatible,
 * ]
 */
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const dbName = jsTest.name();
const collName = "coll";

const rollbackTest = new RollbackTest(dbName);
let primary = rollbackTest.getPrimary();
let primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];
const collNss = primaryColl.getFullName();

const secondary = rollbackTest.getSecondary();
const secondaryDB = secondary.getDB(dbName);

jsTestLog("Do a document write.");
assert.commandWorked(primaryColl.insert({_id: 0, x: 0}, {"writeConcern": {"w": "majority"}}));

// This makes sure the index build on primary hangs before receiving any votes from itself and
// secondary.
IndexBuildTest.pauseIndexBuilds(secondary);
IndexBuildTest.pauseIndexBuilds(primary);

jsTestLog("Start index build.");
const awaitBuild = IndexBuildTest.startIndexBuild(primary, collNss, {i: 1}, {}, [
    ErrorCodes.InterruptedDueToReplStateChange,
]);

jsTestLog("Wait for secondary to reach collection scan phase.");
IndexBuildTest.waitForIndexBuildToScanCollection(secondaryDB, collName, "i_1");

jsTestLog("Wait for primary to reach collection scan phase.");
IndexBuildTest.waitForIndexBuildToScanCollection(primaryDB, collName, "i_1");

rollbackTest.transitionToRollbackOperations();

jsTestLog("Resume index builds.");
IndexBuildTest.resumeIndexBuilds(primary);
IndexBuildTest.resumeIndexBuilds(secondary);

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

let newPrimary = rollbackTest.getPrimary();
let newPrimaryDB = newPrimary.getDB(dbName);
IndexBuildTest.waitForIndexBuildToStop(newPrimaryDB, collName, "i_1");

awaitBuild();

// check to see if the index was successfully created.
IndexBuildTest.assertIndexes(newPrimaryDB[collName], 2, ["_id_", "i_1"]);

rollbackTest.stop();
