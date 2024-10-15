/*
 * Test that the read operations are not killed and their connections are also not
 * closed during step up.
 */
import {waitForCurOpByFailPoint} from "jstests/libs/curop_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const testName = jsTestName();
const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({name: testName, nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];

const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(dbName);
const secondaryAdmin = secondary.getDB("admin");
const secondaryColl = secondaryDB[collName];
const secondaryCollNss = secondaryColl.getFullName();

TestData.dbName = dbName;
TestData.collName = collName;

jsTestLog("1. Do a document write");
assert.commandWorked(primaryColl.insert({_id: 0}, {"writeConcern": {"w": "majority"}}));
rst.awaitReplication();

// It's possible for notPrimaryUnacknowledgedWrites to be non-zero because of mirrored reads during
// initial sync.
let replMetrics = assert.commandWorked(secondaryAdmin.adminCommand({serverStatus: 1})).metrics.repl;
const startingNumNotMasterErrors = replMetrics.network.notPrimaryUnacknowledgedWrites;

// Open a cursor on secondary.
const cursorIdToBeReadAfterStepUp =
    assert.commandWorked(secondaryDB.runCommand({"find": collName, batchSize: 0})).cursor.id;

jsTestLog("2. Start blocking getMore cmd before step up");
const joinGetMoreThread = startParallelShell(() => {
    // Open another cursor on secondary before step up.
    const secondaryDB = db.getSiblingDB(TestData.dbName);
    secondaryDB.getMongo().setSecondaryOk();

    const cursorIdToBeReadDuringStepUp =
        assert.commandWorked(secondaryDB.runCommand({"find": TestData.collName, batchSize: 0}))
            .cursor.id;

    // Enable the fail point for get more cmd.
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "waitAfterPinningCursorBeforeGetMoreBatch", mode: "alwaysOn"}));

    const getMoreRes = assert.commandWorked(secondaryDB.runCommand(
        {"getMore": cursorIdToBeReadDuringStepUp, collection: TestData.collName}));
    assert.docEq([{_id: 0}], getMoreRes.cursor.nextBatch);
}, secondary.port);

// Wait for getmore cmd to reach the fail point.
waitForCurOpByFailPoint(
    secondaryAdmin, secondaryCollNss, "waitAfterPinningCursorBeforeGetMoreBatch");

jsTestLog("2. Start blocking find cmd before step up");
const joinFindThread = startParallelShell(() => {
    const secondaryDB = db.getSiblingDB(TestData.dbName);
    secondaryDB.getMongo().setSecondaryOk();

    // Enable the fail point for find cmd. We are in a parallel shell, so we can't use the helper
    // function. Enable the shard variant of the fail point.
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "shardWaitInFindBeforeMakingBatch", mode: "alwaysOn"}));

    const findRes = assert.commandWorked(secondaryDB.runCommand({"find": TestData.collName}));
    assert.docEq([{_id: 0}], findRes.cursor.firstBatch);
}, secondary.port);

// Wait for find cmd to reach the fail point.
waitForCurOpByFailPoint(secondaryAdmin, secondaryCollNss, "waitInFindBeforeMakingBatch");

jsTestLog("3. Make primary step up");
const joinStepUpThread = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({"replSetStepUp": 100, "force": true}));
}, secondary.port);

// Wait until the step up has started to kill user operations.
checkLog.contains(secondary, "Starting to kill user operations");

// Enable "waitAfterCommandFinishesExecution" fail point to make sure the find and get more
// commands on database 'test' does not complete before step up.
configureFailPoint(secondaryAdmin,
                   "waitAfterCommandFinishesExecution",
                   {ns: secondaryCollNss, commands: ["find", "getMore"]});

jsTestLog("4. Disable fail points");
configureFailPoint(secondaryAdmin, "waitInFindBeforeMakingBatch", {} /* data */, "off");
configureFailPoint(
    secondaryAdmin, "waitAfterPinningCursorBeforeGetMoreBatch", {} /* data */, "off");

// Wait until the secondary transitioned to PRIMARY state.
joinStepUpThread();
rst.waitForState(secondary, ReplSetTest.State.PRIMARY);

// We don't want to check if we have reached "waitAfterCommandFinishesExecution" fail point
// because we already know that the secondary has stepped up successfully. This implies that
// the find and get more commands are still running even after the node stepped up.
configureFailPoint(secondaryAdmin, "waitAfterCommandFinishesExecution", {} /* data */, "off");

// Wait for find & getmore thread to join.
joinGetMoreThread();
joinFindThread();

jsTestLog("5. Start get more cmd after step up");
const getMoreRes = assert.commandWorked(
    secondaryDB.runCommand({"getMore": cursorIdToBeReadAfterStepUp, collection: collName}));
assert.docEq([{_id: 0}], getMoreRes.cursor.nextBatch);

// Validate that no network disconnection happened due to failed unacknowledged operations.
replMetrics = assert.commandWorked(secondaryAdmin.adminCommand({serverStatus: 1})).metrics.repl;
assert.eq(replMetrics.stateTransition.lastStateTransition, "stepUp");
// Should account for find and getmore commands issued before step up.
// TODO (SERVER-85259): Remove references to replMetrics.stateTransition.userOperations*
assert.gte(replMetrics.stateTransition.totalOperationsRunning ||
               replMetrics.stateTransition.userOperationsRunning,
           2);
assert.eq(replMetrics.network.notPrimaryUnacknowledgedWrites, startingNumNotMasterErrors);

rst.stopSet();
