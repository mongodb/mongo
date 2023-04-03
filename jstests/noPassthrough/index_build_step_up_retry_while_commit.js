/**
 * Tests that the step-up skipped record tracker check skips builds that have been concurrently
 * committed.
 *
 * @tags: [
 *   requires_replication,
 * ]
 *
 */
load('jstests/noPassthrough/libs/index_build.js');
load("jstests/libs/fail_point_util.js");
load("jstests/replsets/rslib.js");

(function() {

"use strict";

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];

jsTestLog("Do a document write");
assert.commandWorked(primaryColl.insert({_id: 1, x: 1}, {"writeConcern": {"w": 1}}));

// Clear the log.
assert.commandWorked(primary.adminCommand({clearLog: 'global'}));

// Enable fail point which makes the index build to hang before unregistering after a commit.
const hangBeforeUnregisteringAfterCommit =
    configureFailPoint(primary, 'hangBeforeUnregisteringAfterCommit');

const indexThread = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {x: 1}, {}, ErrorCodes.InterruptedDueToReplStateChange);

jsTestLog("Waiting for index build to hit failpoint");
hangBeforeUnregisteringAfterCommit.wait();

const stepDownThread = startParallelShell(() => {
    jsTestLog("Make primary step down");
    assert.commandWorked(db.adminCommand({"replSetStepDown": 60 * 60, "force": true}));
}, primary.port);

jsTestLog("Waiting for stepdown to complete");
indexThread();
stepDownThread();

waitForState(primary, ReplSetTest.State.SECONDARY);
// Allow the primary to be re-elected, and wait for it.
assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));
rst.getPrimary();

// Wait for the step-up check to be done.
// "Finished performing asynchronous step-up checks on index builds"
checkLog.containsJson(primary, 7508300);

hangBeforeUnregisteringAfterCommit.off();

IndexBuildTest.waitForIndexBuildToStop(primaryDB, primaryColl.getFullName(), "x_1");

IndexBuildTest.assertIndexes(primaryColl, 2, ["_id_", "x_1"], []);

rst.stopSet();
})();
