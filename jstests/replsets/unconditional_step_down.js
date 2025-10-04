/**
 * Tests that unconditional step down terminates writes, but not reads. And, doesn't disconnect
 * the connections if primary is stepping down to secondary.
 */
import {waitForCurOpByFailPoint} from "jstests/libs/curop_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {waitForState} from "jstests/replsets/rslib.js";

const testName = "txnsDuringStepDown";
const dbName = testName;
const collName = "testcoll";
const collNss = dbName + "." + collName;

const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}, {arbiter: true}]});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

let primary;
let secondary;
let primaryDB;

function refreshConnection() {
    primary = rst.getPrimary();
    primaryDB = primary.getDB(dbName);
    secondary = rst.getSecondary();
}

refreshConnection();

jsTestLog("Writing data to collection.");
assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [{_id: "readOp"}]}));
rst.awaitReplication();

const readFailPoint = "waitInFindBeforeMakingBatch";
const writeFailPoint = "hangWithLockDuringBatchInsert";

TestData.dbName = dbName;
TestData.collName = collName;
TestData.readFailPoint = readFailPoint;
TestData.skipRetryOnNetworkError = true;

function runStepDownTest({testMsg, stepDownFn, toRemovedState}) {
    jsTestLog(`Testing step down due to ${testMsg}`);

    // 'toRemovedState' determines whether to tag the connections not to close when
    // primary changes its state to removed.
    toRemovedState = toRemovedState || false;

    // Clears the log before running the test.
    assert.commandWorked(primary.adminCommand({clearLog: "global"}));

    jsTestLog("Enable fail point for namespace '" + collNss + "'");
    // Find command.
    configureFailPoint(primary, readFailPoint, {nss: collNss, shouldCheckForInterrupt: true});
    // Insert command.
    const writeFp = configureFailPoint(primary, writeFailPoint, {nss: collNss, shouldCheckForInterrupt: true});

    let startSafeParallelShell = (func, port) => {
        TestData.func = func;
        let safeFunc = toRemovedState
            ? () => {
                  assert.commandWorked(db.adminCommand({hello: 1, hangUpOnStepDown: false}));
                  TestData.func();
              }
            : func;
        return startParallelShell(safeFunc, port);
    };

    const joinReadThread = startSafeParallelShell(() => {
        jsTestLog("Start blocking find cmd before step down");
        let findRes = assert.commandWorked(db.getSiblingDB(TestData.dbName).runCommand({"find": TestData.collName}));
        assert.eq(findRes.cursor.firstBatch.length, 1);
    }, primary.port);

    const joinWriteThread = startSafeParallelShell(() => {
        jsTestLog("Start blocking insert cmd before step down");
        assert.commandFailedWithCode(
            db.getSiblingDB(TestData.dbName)[TestData.collName].insert([{val: "writeOp1"}]),
            ErrorCodes.InterruptedDueToReplStateChange,
        );
    }, primary.port);

    // A failpoint to hang in the middle of a 'checkLog' command. This is used to synchronize
    // the 'joinUnblockStepDown' thread with 'stepDown'.
    const hangFp = configureFailPoint(primary, "hangInGetLog");
    const joinUnblockStepDown = startSafeParallelShell(() => {
        jsTestLog("Wait for step down to start killing operations");
        checkLog.contains(db, "Starting to kill user operations");

        jsTestLog("Unblock step down");
        // Turn off fail point on find cmd to allow step down to continue. Hardcode the use of the
        // shard failpoint here since we are in a parallel shell.
        assert.commandWorked(db.adminCommand({configureFailPoint: "shardWaitInFindBeforeMakingBatch", mode: "off"}));
    }, primary.port);

    jsTestLog("Wait for find cmd to reach the fail point");
    waitForCurOpByFailPoint(primaryDB, collNss, readFailPoint);

    jsTestLog("Wait for write cmd to reach the fail point");
    waitForCurOpByFailPoint(primaryDB, collNss, writeFailPoint);

    // Make sure the 'joinUnblockStepDown' thread has connected before initiating stepdown.
    hangFp.wait();
    hangFp.off();

    let res = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
    assert(
        res.electionCandidateMetrics,
        () => "Response should have an 'electionCandidateMetrics' field: " + tojson(res),
    );

    jsTestLog("Trigger step down");
    let oldConfig = stepDownFn();

    // Waits for all threads to join.
    joinUnblockStepDown();
    joinReadThread();
    joinWriteThread();

    // Wait till the primary stepped down to primary.
    waitForState(primary, toRemovedState ? ReplSetTest.State.REMOVED : ReplSetTest.State.SECONDARY);

    writeFp.off();

    // Check that the 'electionCandidateMetrics' section of the replSetGetStatus response has been
    // cleared, since the node is no longer primary.
    if (!toRemovedState) {
        res = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
        assert(
            !res.electionCandidateMetrics,
            () => "Response should not have an 'electionCandidateMetrics' field: " + tojson(res),
        );
    }

    // Get the new primary.
    refreshConnection();
}

function runStepsDowntoRemoved(params) {
    let oldConfigBeforeTest = rst.getReplSetConfigFromNode();

    // Run the test.
    params["toRemovedState"] = true;
    runStepDownTest(params);
    oldConfigBeforeTest.version = ++rst.getReplSetConfigFromNode().version;

    // On exit, add the removed node back to replica set.
    assert.commandWorked(primary.adminCommand({replSetReconfig: oldConfigBeforeTest, force: true}));
    refreshConnection();
}

runStepDownTest({
    testMsg: "reconfig command",
    stepDownFn: () => {
        let newConfig = rst.getReplSetConfigFromNode();

        let oldMasterId = rst.getNodeId(primary);
        let newMasterId = rst.getNodeId(secondary);

        newConfig.members[oldMasterId].priority = 0;
        newConfig.members[newMasterId].priority = 1;
        newConfig.version++;

        // Run it on primary
        assert.commandWorked(primary.adminCommand({replSetReconfig: newConfig, force: true}));
    },
});

runStepDownTest({
    testMsg: "reconfig via heartbeat",
    stepDownFn: () => {
        let newConfig = rst.getReplSetConfigFromNode();

        let oldMasterId = rst.getNodeId(primary);
        let newMasterId = rst.getNodeId(secondary);

        newConfig.members[oldMasterId].priority = 0;
        newConfig.members[newMasterId].priority = 1;
        newConfig.version++;

        // Run it on secondary
        assert.commandWorked(secondary.adminCommand({replSetReconfig: newConfig, force: true}));
    },
});

runStepsDowntoRemoved({
    testMsg: "reconfig via heartbeat - primary to removed",
    stepDownFn: () => {
        let newConfig = rst.getReplSetConfigFromNode();

        let oldMasterId = rst.getNodeId(primary);
        let newMasterId = rst.getNodeId(secondary);

        newConfig.members[newMasterId].priority = 1;
        // Remove the current primary from the config
        newConfig.members.splice(oldMasterId, 1);
        newConfig.version++;

        // Run it on secondary
        assert.commandWorked(secondary.adminCommand({replSetReconfig: newConfig, force: true}));
    },
});

runStepDownTest({
    testMsg: "stepdown via heartbeat",
    stepDownFn: () => {
        let newConfig = rst.getReplSetConfigFromNode();
        let newMasterId = rst.getNodeId(secondary);

        newConfig.members[newMasterId].priority = 2;
        newConfig.version++;

        // Run it on primary
        assert.commandWorked(primary.adminCommand({replSetReconfig: newConfig, force: false}));

        // Now, step up the secondary which will make the current primary to step down.
        rst.stepUp(secondary);
    },
});

rst.stopSet();
