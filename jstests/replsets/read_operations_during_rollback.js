/*
 * This test makes sure 'find' and 'getMore' commands fail correctly during rollback.
 */
(function() {
    "use strict";

    load("jstests/replsets/libs/rollback_test.js");

    const dbName = "test";
    const collName = "coll";

    let setFailPoint = (node, failpoint) => {
        jsTestLog("Setting fail point " + failpoint);
        assert.commandWorked(node.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn"}));
    };

    let clearFailPoint = (node, failpoint) => {
        jsTestLog("Clearing fail point " + failpoint);
        assert.commandWorked(node.adminCommand({configureFailPoint: failpoint, mode: "off"}));
    };

    // Set up Rollback Test.
    let rollbackTest = new RollbackTest();

    // Insert a document to be read later.
    assert.commandWorked(rollbackTest.getPrimary().getDB(dbName)[collName].insert({}));

    let rollbackNode = rollbackTest.transitionToRollbackOperations();

    setFailPoint(rollbackNode, "rollbackHangAfterTransitionToRollback");

    setFailPoint(rollbackNode, "GetMoreHangBeforeReadLock");

    const joinGetMoreThread = startParallelShell(() => {
        db.getMongo().setSlaveOk();
        const cursorID =
            assert.commandWorked(db.runCommand({"find": "coll", batchSize: 0})).cursor.id;
        let res = assert.throws(function() {
            db.runCommand({"getMore": cursorID, collection: "coll"});
        }, [], "network error");
        // Make sure the connection of an outstanding read operation gets closed during rollback
        // even though the read was started before rollback.
        assert.includes(res.toString(), "network error while attempting to run command");
    }, rollbackNode.port);

    const cursorIdToBeReadDuringRollback =
        assert
            .commandWorked(rollbackNode.getDB(dbName).runCommand({"find": collName, batchSize: 0}))
            .cursor.id;

    // Wait for 'getMore' to hang.
    checkLog.contains(rollbackNode, "GetMoreHangBeforeReadLock fail point enabled.");

    // Start rollback.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();

    jsTestLog("Reconnecting to " + rollbackNode.host + " after rollback");
    reconnect(rollbackNode.getDB(dbName));

    // Wait for rollback to hang.
    checkLog.contains(rollbackNode, "rollbackHangAfterTransitionToRollback fail point enabled.");

    clearFailPoint(rollbackNode, "GetMoreHangBeforeReadLock");

    jsTestLog("Wait for 'getMore' thread to join.");
    joinGetMoreThread();

    jsTestLog("Reading during rollback.");
    // Make sure that read operations fail during rollback.
    assert.commandFailedWithCode(rollbackNode.getDB(dbName).runCommand({"find": collName}),
                                 ErrorCodes.NotMasterOrSecondary);
    assert.commandFailedWithCode(
        rollbackNode.getDB(dbName).runCommand(
            {"getMore": cursorIdToBeReadDuringRollback, collection: collName}),
        ErrorCodes.NotMasterOrSecondary);

    // Disable the best-effort check for primary-ness in the service entry point, so that we
    // exercise the real check for primary-ness in 'find' and 'getMore' commands.
    setFailPoint(rollbackNode, "skipCheckingForNotMasterInCommandDispatch");
    jsTestLog("Reading during rollback (again with command dispatch checks disabled).");
    assert.commandFailedWithCode(rollbackNode.getDB(dbName).runCommand({"find": collName}),
                                 ErrorCodes.NotMasterOrSecondary);
    assert.commandFailedWithCode(
        rollbackNode.getDB(dbName).runCommand(
            {"getMore": cursorIdToBeReadDuringRollback, collection: collName}),
        ErrorCodes.NotMasterOrSecondary);

    clearFailPoint(rollbackNode, "rollbackHangAfterTransitionToRollback");

    rollbackTest.transitionToSteadyStateOperations();

    // Check the replica set.
    rollbackTest.stop();
}());
