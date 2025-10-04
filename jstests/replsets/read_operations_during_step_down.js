/*
 * Test that the read operations are not killed and their connections are also not
 * closed during step down.
 */
import {waitForCurOpByFailPoint} from "jstests/libs/curop_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const testName = "readOpsDuringStepDown";
const dbName = "test";
const collName = "coll";

let rst = new ReplSetTest({name: testName, nodes: [{}, {rsConfig: {priority: 0}}]});
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
assert.commandWorked(primaryColl.insert({_id: 0}, {"writeConcern": {"w": "majority"}}));
rst.awaitReplication();

// Open a cursor on primary.
const cursorIdToBeReadAfterStepDown = assert.commandWorked(primaryDB.runCommand({"find": collName, batchSize: 0}))
    .cursor.id;

jsTestLog("2. Start blocking getMore cmd before step down");
const joinGetMoreThread = startParallelShell(() => {
    // Open another cursor on primary before step down.
    const primaryDB = db.getSiblingDB(TestData.dbName);
    const cursorIdToBeReadDuringStepDown = assert.commandWorked(
        primaryDB.runCommand({"find": TestData.collName, batchSize: 0}),
    ).cursor.id;

    // Enable the fail point for get more cmd.
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "waitAfterPinningCursorBeforeGetMoreBatch", mode: "alwaysOn"}),
    );

    getMoreRes = assert.commandWorked(
        primaryDB.runCommand({"getMore": cursorIdToBeReadDuringStepDown, collection: TestData.collName}),
    );
    assert.docEq([{_id: 0}], getMoreRes.cursor.nextBatch);
}, primary.port);

// Wait for getmore cmd to reach the fail point.
waitForCurOpByFailPoint(primaryAdmin, collNss, "waitAfterPinningCursorBeforeGetMoreBatch");

jsTestLog("2. Start blocking find cmd before step down");
const joinFindThread = startParallelShell(() => {
    // Enable the fail point for find cmd. We know this is a replica set, so enable
    // "shardWaitInFindBeforeMakingBatch" (helper function configureFailPoint() cannot be used
    // inside a parallel shell).
    assert.commandWorked(db.adminCommand({configureFailPoint: "shardWaitInFindBeforeMakingBatch", mode: "alwaysOn"}));

    let findRes = assert.commandWorked(db.getSiblingDB(TestData.dbName).runCommand({"find": TestData.collName}));
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
const failPointAfterCommand = configureFailPoint(primaryAdmin, "waitAfterCommandFinishesExecution", {
    ns: collNss,
    commands: ["find", "getMore"],
});

jsTestLog("4. Disable fail points");
configureFailPoint(primaryAdmin, "waitInFindBeforeMakingBatch", {} /* data */, "off");
configureFailPoint(primaryAdmin, "waitAfterPinningCursorBeforeGetMoreBatch", {} /* data */, "off");

// Wait until the primary transitioned to SECONDARY state.
joinStepDownThread();
rst.awaitSecondaryNodes(null, [primary]);

// We don't want to check if we have reached "waitAfterCommandFinishesExecution" fail point
// because we already know that the primary has stepped down successfully. This implies that
// the find and get more commands are still running even after the node stepped down.
failPointAfterCommand.off();
// Wait for find & getmore thread to join.
joinGetMoreThread();
joinFindThread();

jsTestLog("5. Start get more cmd after step down");
var getMoreRes = assert.commandWorked(
    primaryDB.runCommand({"getMore": cursorIdToBeReadAfterStepDown, collection: collName}),
);
assert.docEq([{_id: 0}], getMoreRes.cursor.nextBatch);

// Validate that no network disconnection happened due to failed unacknowledged operations.
const replMetrics = assert.commandWorked(primaryAdmin.adminCommand({serverStatus: 1})).metrics.repl;
assert.eq(replMetrics.stateTransition.lastStateTransition, "stepDown");
// Should account for find and getmore commands issued before step down.
// TODO (SERVER-85259): Remove references to replMetrics.stateTransition.userOperations*
assert.gte(replMetrics.stateTransition.totalOperationsRunning || replMetrics.stateTransition.userOperationsRunning, 2);
assert.eq(replMetrics.network.notPrimaryUnacknowledgedWrites, 0);

rst.stopSet();
