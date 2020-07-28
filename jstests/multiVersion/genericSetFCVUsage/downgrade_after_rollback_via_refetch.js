// When enableMajorityReadConcern=false, a node transitions from ROLLBACK to RECOVERING with an
// unstable checkpoint with appliedThrough set to the common point. Test that if the node crashes
// and restarts with the downgraded version before its next stable checkpoint, then oplog entries
// after the common point are replayed.
(function() {
"use strict";

load("jstests/replsets/libs/rollback_test.js");

TestData.rollbackShutdowns = true;
TestData.allowUncleanShutdowns = true;
let name = "downgrade_after_rollback_via_refetch";
let dbName = "test";
let sourceCollName = "coll";

function testDowngrade(enableMajorityReadConcern, downgradeVersion) {
    jsTest.log("Test downgrade with enableMajorityReadConcern=" + enableMajorityReadConcern +
               " and downgradeVersion: " + downgradeVersion);
    const downgradeFCV = binVersionToFCV(downgradeVersion);
    // Set up Rollback Test.
    let replTest = new ReplSetTest(
        {name, nodes: 3, useBridge: true, nodeOptions: {enableMajorityReadConcern: "false"}});
    replTest.startSet();
    let config = replTest.getReplSetConfig();
    config.members[2].priority = 0;
    config.settings = {chainingAllowed: false};
    replTest.initiateWithHighElectionTimeout(config);
    let rollbackTest = new RollbackTest(name, replTest);

    // Set the featureCompatibilityVersion to the downgraded version, so that we can downgrade
    // the rollback node.
    assert.commandWorked(
        rollbackTest.getPrimary().adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    let rollbackNode = rollbackTest.transitionToRollbackOperations();

    // Turn off stable checkpoints on the rollback node.
    assert.commandWorked(
        rollbackNode.adminCommand({configureFailPoint: "disableSnapshotting", mode: "alwaysOn"}));

    // Wait for a rollback to finish.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    // Replicate a new operation to the rollback node. Replication is disabled on the tiebreaker
    // node, so a successful majority write guarantees the write has replicated to the rollback
    // node.
    assert.commandWorked(rollbackTest.getPrimary().getDB(dbName)[sourceCollName].insert(
        {_id: 0}, {writeConcern: {w: "majority"}}));
    assert.eq(rollbackNode.getDB(dbName)[sourceCollName].find({_id: 0}).itcount(), 1);

    // SERVER-47219: The following unclean shutdown followed by a restart into downgradeVersion is
    // not a legal downgrade scenario. However, this illegal downgrade is only prevented when a
    // change across versions requires it. There exists a patch for this test in v4.4 when illegal
    // downgrades are prevented. The patch for that case however requires demonstrating the illegal
    // downgrade is prevented as expected. Applying that here results in a hang. The testing
    // infrastructure for running mongod processes in sufficiently complex scenarios, cannot express
    // both expecting a startup to fail with an error as well as failing immediately if startup
    // succeeds.
    //
    // If this test starts failing on the restart below due to an illegal downgrade, forward-porting
    // the v4.4 patch for SERVER-47219 should be the first thing to try.
    //
    // Kill the rollback node and restart it on the downgraded version.
    rollbackTest.restartNode(
        0, 9, {binVersion: downgradeVersion, enableMajorityReadConcern: enableMajorityReadConcern});
    replTest.awaitSecondaryNodes();

    // The rollback node should replay the new operation.
    rollbackNode = rollbackTest.getSecondary();
    assert.eq(rollbackNode.getDB(dbName)[sourceCollName].find({_id: 0}).itcount(), 1);

    rollbackTest.stop();
}

testDowngrade("true", "last-lts");
testDowngrade("false", "last-lts");
testDowngrade("true", "last-continuous");
testDowngrade("false", "last-continuous");
})();
