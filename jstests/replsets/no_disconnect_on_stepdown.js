/**
 * Tests that stepdown terminates writes, but does not disconnect connections.
 */
(function() {
    "use strict";

    load("jstests/libs/check_log.js");

    const rst = new ReplSetTest(
        {nodes: [{setParameter: {closeConnectionsOnStepdown: false}}, {rsConfig: {priority: 0}}]});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const primaryAdmin = primary.getDB("admin");
    // We need a separate connection to avoid interference with the ReplSetTestMechanism.
    const primaryDataConn = new Mongo(primary.host);
    const primaryDb = primaryDataConn.getDB("test");
    const collname = "no_disconnect_on_stepdown";
    const coll = primaryDb[collname];

    // Never retry on network error, because this test needs to detect the network error.
    TestData.skipRetryOnNetworkError = true;

    // Legacy writes will still disconnect, so don't use them.
    primaryDataConn.forceWriteMode('commands');

    assert.commandWorked(coll.insert([{_id: 'deleteme'}, {_id: 'updateme'}, {_id: 'findme'}]));
    rst.awaitReplication();

    jsTestLog("Stepping down with no command in progress.  Should not disconnect.");
    // If the 'primary' connection is broken on stepdown, this command will fail.
    assert.commandWorked(primaryAdmin.adminCommand({replSetStepDown: 60, force: true}));
    rst.waitForState(primary, ReplSetTest.State.SECONDARY);
    // If the 'primaryDataConn' connection was broken during stepdown, this command will fail.
    assert.commandWorked(primaryDb.adminCommand({ping: 1}));
    // Allow the primary to be re-elected, and wait for it.
    assert.commandWorked(primaryAdmin.adminCommand({replSetFreeze: 0}));
    rst.getPrimary();

    function runStepDownTest({description, failpoint, operation, errorCode}) {
        jsTestLog(`Trying ${description} on a stepping-down primary`);
        assert.commandWorked(
            primaryAdmin.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn"}));

        errorCode = errorCode || ErrorCodes.InterruptedDueToStepDown;
        const writeCommand = `db.getMongo().forceWriteMode("commands");
                              assert.commandFailedWithCode(${operation}, ${errorCode});
                              assert.commandWorked(db.adminCommand({ping:1}));`;

        const waitForShell = startParallelShell(writeCommand, primary.port);
        checkLog.contains(primary, failpoint + " fail point enabled");
        assert.commandWorked(primaryAdmin.adminCommand({replSetStepDown: 60, force: true}));
        rst.waitForState(primary, ReplSetTest.State.SECONDARY);
        assert.commandWorked(
            primaryAdmin.adminCommand({configureFailPoint: failpoint, mode: "off"}));
        try {
            waitForShell();
        } catch (ex) {
            print("Failed trying to write or ping in " + description + ", possibly disconnected.");
            throw ex;
        }

        // Allow the primary to be re-elected, and wait for it.
        assert.commandWorked(primaryAdmin.adminCommand({replSetFreeze: 0}));
        rst.getPrimary();
    }
    runStepDownTest({
        description: "insert",
        failpoint: "hangDuringBatchInsert",
        operation: "db['" + collname + "'].insert({id:0})"
    });
    runStepDownTest({
        description: "update",
        failpoint: "hangDuringBatchUpdate",
        operation: "db['" + collname + "'].update({_id: 'updateme'}, {'$set': {x: 1}})"
    });
    runStepDownTest({
        description: "remove",
        failpoint: "hangDuringBatchRemove",
        operation: "db['" + collname + "'].remove({_id: 'deleteme'}, {'$set': {x: 1}})"
    });
    runStepDownTest({
        description: "linearizable read",
        failpoint: "hangBeforeLinearizableReadConcern",
        operation: "db.runCommand({find: '" + collname +
            "', filter: {'_id': 'findme'}, readConcern: {level: 'linearizable'}})",
    });
    rst.stopSet();
})();
