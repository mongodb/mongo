/**
 * This tests that rollback has the ability to retry the 'find' command on the sync source's
 * oplog, which may fail due to transient network errors. It uses the 'failCommand' failpoint
 * to simulate exactly two network failures, so that common point resolution can succeed on the
 * third attempt.
 */

(function() {
    "use strict";
    load("jstests/replsets/libs/rollback_test.js");
    load("jstests/libs/check_log.js");

    const testName = "rollback_remote_cursor_retry";
    const dbName = testName;

    const rollbackTest = new RollbackTest(testName);

    // Reduce the frequency of heartbeats so that they are unlikely to be included in the
    // count for the 'failCommand' failpoint.
    // TODO SERVER-35004: Remove this reconfig.
    jsTestLog("Increasing heartbeat interval to 30 seconds and election timeout to 60 seconds.");

    const replSet = rollbackTest.getTestFixture();
    const conf = replSet.getReplSetConfigFromNode();

    conf.settings.heartbeatIntervalMillis = 30 * 1000;
    conf.settings.electionTimeoutMillis = 60 * 1000;
    conf.version = conf.version + 1;
    reconfig(replSet, conf, true);

    replSet.awaitReplication();

    const rollbackNode = rollbackTest.transitionToRollbackOperations();
    const syncSource = rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

    // This failpoint is used to make sure that we have started rollback before turning on
    // 'failCommand'. Otherwise, we would be failing the 'find' command that we issue against
    // the sync source before we decide to go into rollback.
    assert.commandWorked(rollbackNode.adminCommand(
        {configureFailPoint: "rollbackHangBeforeStart", mode: "alwaysOn"}));

    rollbackTest.transitionToSyncSourceOperationsDuringRollback();

    // Ensure that we've hit the failpoint before moving on.
    checkLog.contains(rollbackNode, "rollback - rollbackHangBeforeStart fail point enabled");

    // Fail the 'find' command exactly twice.
    // TODO SERVER-35004 Configure this failpoint to only fail the 'find' command.
    jsTestLog("Failing the next two 'find' commands.");
    assert.commandWorked(syncSource.adminCommand(
        {configureFailPoint: "failCommand", data: {closeConnection: true}, mode: {times: 2}}));

    // Let rollback proceed.
    assert.commandWorked(
        rollbackNode.adminCommand({configureFailPoint: "rollbackHangBeforeStart", mode: "off"}));

    rollbackTest.transitionToSteadyStateOperations();
    rollbackTest.stop();

})();
