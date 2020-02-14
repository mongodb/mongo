/**
 * This test demonstrates that rollback via refetch always increments the rollback id as soon as it
 * resolves the common point and before proceeding with other operations.
 *
 * This is a regression test that makes sure we avoid the scenario where we truncate our oplog (at
 * which point the rollback is effectively finished), then shut down uncleanly before we get a
 * chance to update the rollbackId.
 *
 * @tags: [requires_journaling]
 */

(function() {
    "use strict";
    load("jstests/replsets/libs/rollback_test.js");
    load("jstests/replsets/rslib.js");

    const name = jsTestName();
    TestData.allowUncleanShutdowns = true;

    jsTest.log("Set up a RollbackTest with enableMajorityReadConcern=false");
    const rst = new ReplSetTest({
        name,
        nodes: [{}, {}, {rsConfig: {arbiterOnly: true}}],
        useBridge: true,
        nodeOptions: {enableMajorityReadConcern: "false"},
        settings: {chainingAllowed: false}
    });

    rst.startSet();
    rst.initiateWithHighElectionTimeout();

    const rollbackTest = new RollbackTest(name, rst);
    const rollbackNode = rollbackTest.transitionToRollbackOperations();

    const baseRBID = assert.commandWorked(rollbackNode.adminCommand("replSetGetRBID")).rbid;

    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

    jsTestLog("Make rollback-via-refetch exit early after truncating the oplog");
    assert.commandWorked(rollbackNode.adminCommand(
        {configureFailPoint: "rollbackExitEarlyAfterCollectionDrop", mode: "alwaysOn"}));

    rollbackTest.transitionToSyncSourceOperationsDuringRollback();

    jsTestLog("Wait until we hit the failpoint");
    checkLog.contains(rollbackNode, "rollbackExitEarlyAfterCollectionDrop fail point enabled");

    // Check that the RBID has still managed to advance.
    // Looking at the RBID directly is our first line of defense.
    assert.eq(baseRBID + 1, assert.commandWorked(rollbackNode.adminCommand("replSetGetRBID")).rbid);

    assert.commandWorked(rollbackNode.adminCommand(
        {configureFailPoint: "rollbackExitEarlyAfterCollectionDrop", mode: "off"}));

    // Verify that the node can rejoin the set as normal.
    rollbackTest.transitionToSteadyStateOperations();
    rollbackTest.stop();
}());