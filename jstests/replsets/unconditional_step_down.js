/**
 * Tests that unconditional step down terminates writes, but not reads. And, doesn't disconnect
 * the connections if primary is stepping down to secondary.
 */
(function() {
    "use strict";

    load("jstests/libs/curop_helpers.js");  // for waitForCurOpByFailPoint().

    const testName = "txnsDuringStepDown";
    const dbName = testName;
    const collName = "testcoll";
    const collNss = dbName + '.' + collName;

    const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}, {arbiter: true}]});
    rst.startSet();
    rst.initiate();

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
    assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [{_id: 'readOp'}]}));
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
        assert.commandWorked(primary.adminCommand({clearLog: 'global'}));

        jsTestLog("Enable fail point for namespace '" + collNss + "'");
        // Find command.
        assert.commandWorked(primary.adminCommand({
            configureFailPoint: readFailPoint,
            data: {nss: collNss, shouldCheckForInterrupt: true},
            mode: "alwaysOn"
        }));
        // Insert command.
        assert.commandWorked(primary.adminCommand({
            configureFailPoint: writeFailPoint,
            data: {nss: collNss, shouldCheckForInterrupt: true},
            mode: "alwaysOn"
        }));

        var startSafeParallelShell = (func, port) => {
            TestData.func = func;
            var safeFunc = (toRemovedState) ? () => {
                assert.commandWorked(db.adminCommand({isMaster: 1, hangUpOnStepDown: false}));
                TestData.func();
            } : func;
            return startParallelShell(safeFunc, port);
        };

        const joinReadThread = startSafeParallelShell(() => {
            jsTestLog("Start blocking find cmd before step down");
            var findRes = assert.commandWorked(
                db.getSiblingDB(TestData.dbName).runCommand({"find": TestData.collName}));
            assert.eq(findRes.cursor.firstBatch.length, 1);
        }, primary.port);

        const joinWriteThread = startSafeParallelShell(() => {
            jsTestLog("Start blocking insert cmd before step down");
            assert.commandFailedWithCode(
                db.getSiblingDB(TestData.dbName)[TestData.collName].insert([{val: 'writeOp1'}]),
                ErrorCodes.InterruptedDueToReplStateChange);
        }, primary.port);

        const joinUnblockStepDown = startSafeParallelShell(() => {
            load("jstests/libs/check_log.js");

            jsTestLog("Wait for step down to start killing operations");
            checkLog.contains(db, "Starting to kill user operations");

            jsTestLog("Unblock step down");
            // Turn off fail point on find cmd to allow step down to continue.
            assert.commandWorked(
                db.adminCommand({configureFailPoint: TestData.readFailPoint, mode: "off"}));
        }, primary.port);

        jsTestLog("Wait for find cmd to reach the fail point");
        waitForCurOpByFailPoint(primaryDB, collNss, readFailPoint);

        jsTestLog("Wait for write cmd to reach the fail point");
        waitForCurOpByFailPoint(primaryDB, collNss, writeFailPoint);

        jsTestLog("Trigger step down");
        var oldConfig = stepDownFn();

        // Waits for all threads to join.
        joinUnblockStepDown();
        joinReadThread();
        joinWriteThread();

        // Wait till the primary stepped down to primary.
        waitForState(primary,
                     (toRemovedState) ? ReplSetTest.State.REMOVED : ReplSetTest.State.SECONDARY);

        assert.commandWorked(
            primary.adminCommand({configureFailPoint: writeFailPoint, mode: "off"}));
        // Get the new primary.
        refreshConnection();
    }

    function runStepsDowntoRemoved(params) {
        var oldConfigBeforeTest = rst.getReplSetConfigFromNode();

        // Run the test.
        params["toRemovedState"] = true;
        runStepDownTest(params);
        oldConfigBeforeTest.version = ++(rst.getReplSetConfigFromNode().version);

        // On exit, add the removed node back to replica set.
        assert.commandWorked(
            primary.adminCommand({replSetReconfig: oldConfigBeforeTest, force: true}));
        refreshConnection();
    }

    runStepDownTest({
        testMsg: "reconfig command",
        stepDownFn: () => {
            load("./jstests/replsets/rslib.js");
            var newConfig = rst.getReplSetConfigFromNode();

            var oldMasterId = rst.getNodeId(primary);
            var newMasterId = rst.getNodeId(secondary);

            newConfig.members[oldMasterId].priority = 0;
            newConfig.members[newMasterId].priority = 1;
            newConfig.version++;

            // Run it on primary
            assert.commandWorked(primary.adminCommand({replSetReconfig: newConfig, force: true}));
        }
    });

    runStepDownTest({
        testMsg: "reconfig via heartbeat",
        stepDownFn: () => {
            load("./jstests/replsets/rslib.js");
            var newConfig = rst.getReplSetConfigFromNode();

            var oldMasterId = rst.getNodeId(primary);
            var newMasterId = rst.getNodeId(secondary);

            newConfig.members[oldMasterId].priority = 0;
            newConfig.members[newMasterId].priority = 1;
            newConfig.version++;

            // Run it on secondary
            assert.commandWorked(secondary.adminCommand({replSetReconfig: newConfig, force: true}));
        }
    });

    runStepsDowntoRemoved({
        testMsg: "reconfig via heartbeat - primary to removed",
        stepDownFn: () => {
            load("./jstests/replsets/rslib.js");
            var newConfig = rst.getReplSetConfigFromNode();

            var oldMasterId = rst.getNodeId(primary);
            var newMasterId = rst.getNodeId(secondary);

            newConfig.members[newMasterId].priority = 1;
            // Remove the current primary from the config
            newConfig.members.splice(oldMasterId, 1);
            newConfig.version++;

            // Run it on secondary
            assert.commandWorked(secondary.adminCommand({replSetReconfig: newConfig, force: true}));
        }
    });

    runStepDownTest({
        testMsg: "stepdown via heartbeat",
        stepDownFn: () => {
            load("./jstests/replsets/rslib.js");
            var newConfig = rst.getReplSetConfigFromNode();

            var newMasterId = rst.getNodeId(secondary);

            newConfig.members[newMasterId].priority = 2;
            newConfig.version++;

            // Run it on primary
            assert.commandWorked(primary.adminCommand({replSetReconfig: newConfig, force: false}));

            // Now, step up the secondary which will make the current primary to step down.
            rst.stepUp(secondary);
        }
    });

    rst.stopSet();
})();
