/*
 * Test that the read operations are not killed and their connections are also not
 * closed during step down.
 */
load("jstests/libs/check_log.js");
load('jstests/libs/parallelTester.js');
load("jstests/libs/curop_helpers.js");  // for waitForCurOpByFailPoint().
load("jstests/replsets/rslib.js");

(function() {

"use strict";

const testName = "readOpsDuringStepDown";
const dbName = "test";
const collName = "coll";

var rst = new ReplSetTest({name: testName, nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryAdmin = primary.getDB("admin");
const primaryColl = primaryDB[collName];
const collNss = primaryColl.getFullName();

TestData.dbName = dbName;
TestData.collName = collName;

jsTestLog("1. Do a document write");
assert.commandWorked(
        primaryColl.insert({_id: 0}, {"writeConcern": {"w": "majority"}}));
rst.awaitReplication();

// Open a cursor on primary.
const cursorIdToBeReadAfterStepDown =
    assert.commandWorked(primaryDB.runCommand({"find": collName, batchSize: 0})).cursor.id;

jsTestLog("2. Start blocking getMore cmd before step down");
const joinGetMoreThread = startParallelShell(() => {
    // Open another cursor on primary before step down.
    primaryDB = db.getSiblingDB(TestData.dbName);
    const cursorIdToBeReadDuringStepDown =
        assert.commandWorked(primaryDB.runCommand({"find": TestData.collName, batchSize: 0}))
            .cursor.id;

    // Enable the fail point for get more cmd.
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "waitAfterPinningCursorBeforeGetMoreBatch", mode: "alwaysOn"}));

    getMoreRes = assert.commandWorked(primaryDB.runCommand(
        {"getMore": cursorIdToBeReadDuringStepDown, collection: TestData.collName}));
    assert.docEq([{_id: 0}], getMoreRes.cursor.nextBatch);
}, primary.port);

// Wait for getmore cmd to reach the fail point.
waitForCurOpByFailPoint(primaryAdmin, collNss, "waitAfterPinningCursorBeforeGetMoreBatch");

jsTestLog("2. Start blocking find cmd before step down");
const joinFindThread = startParallelShell(() => {
    // Enable the fail point for find cmd.
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "waitInFindBeforeMakingBatch", mode: "alwaysOn"}));

    var findRes = assert.commandWorked(
        db.getSiblingDB(TestData.dbName).runCommand({"find": TestData.collName}));
    assert.docEq([{_id: 0}], findRes.cursor.firstBatch);
}, primary.port);

// Wait for find cmd to reach the fail point.
waitForCurOpByFailPoint(primaryAdmin, collNss, "waitInFindBeforeMakingBatch");

jsTestLog("3. Make primary step down");
const joinStepDownThread = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({"replSetStepDown": 100, "force": true}));
}, primary.port);

// Wait until the step down has started to kill user operations.
checkLog.contains(primary, "Starting to kill user operations");

// Enable "waitAfterCommandFinishesExecution" fail point to make sure the find and get more
// commands on database 'test' does not complete before step down.
setFailPoint(primaryAdmin,
             "waitAfterCommandFinishesExecution",
             {ns: collNss, commands: ["find", "getMore"]});

jsTestLog("4. Disable fail points");
clearFailPoint(primaryAdmin, "waitInFindBeforeMakingBatch");
clearFailPoint(primaryAdmin, "waitAfterPinningCursorBeforeGetMoreBatch");

// Wait until the primary transitioned to SECONDARY state.
joinStepDownThread();
rst.waitForState(primary, ReplSetTest.State.SECONDARY);

// We don't want to check if we have reached "waitAfterCommandFinishesExecution" fail point
// because we already know that the primary has stepped down successfully. This implies that
// the find and get more commands are still running even after the node stepped down.
clearFailPoint(primaryAdmin, "waitAfterCommandFinishesExecution");

// Wait for find & getmore thread to join.
joinGetMoreThread();
joinFindThread();

jsTestLog("5. Start get more cmd after step down");
var getMoreRes = assert.commandWorked(
    primaryDB.runCommand({"getMore": cursorIdToBeReadAfterStepDown, collection: collName}));
assert.docEq([{_id: 0}], getMoreRes.cursor.nextBatch);

// Validate that no operations got killed on step down and no network disconnection happened due
// to failed unacknowledged operations.
const replMetrics = assert.commandWorked(primaryAdmin.adminCommand({serverStatus: 1})).metrics.repl;
assert.eq(replMetrics.stateTransition.lastStateTransition, "stepDown");
assert.eq(replMetrics.stateTransition.userOperationsKilled, 0);
// Should account for find and getmore commands issued before step down.
assert.gte(replMetrics.stateTransition.userOperationsRunning, 2);
assert.eq(replMetrics.network.notMasterUnacknowledgedWrites, 0);

rst.stopSet();
})();
