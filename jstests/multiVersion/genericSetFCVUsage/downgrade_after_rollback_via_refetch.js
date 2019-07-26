// When enableMajorityReadConcern=false, a node transitions from ROLLBACK to RECOVERING with an
// unstable checkpoint with appliedThrough set to the common point. Test that if the node crashes
// and restarts with the last-stable version before its next stable checkpoint, then oplog entries
// after the common point are replayed.
(function() {
"use strict";

load("jstests/libs/feature_compatibility_version.js");
load("jstests/replsets/libs/rollback_test.js");

TestData.rollbackShutdowns = true;
TestData.allowUncleanShutdowns = true;
let name = "downgrade_after_rollback_via_refetch";
let dbName = "test";
let sourceCollName = "coll";

function testDowngrade(enableMajorityReadConcern) {
    jsTest.log("Test downgrade with enableMajorityReadConcern=" + enableMajorityReadConcern);

    // Set up Rollback Test.
    let replTest = new ReplSetTest(
        {name, nodes: 3, useBridge: true, nodeOptions: {enableMajorityReadConcern: "false"}});
    replTest.startSet();
    let config = replTest.getReplSetConfig();
    config.members[2].priority = 0;
    config.settings = {chainingAllowed: false};
    replTest.initiate(config);
    let rollbackTest = new RollbackTest(name, replTest);

    // Set the featureCompatibilityVersion to the last-stable version, so that we can downgrade
    // the rollback node.
    assert.commandWorked(
        rollbackTest.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

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

    // Kill the rollback node and restart it on the last-stable version.
    rollbackTest.restartNode(
        0, 9, {binVersion: "last-stable", enableMajorityReadConcern: enableMajorityReadConcern});
    replTest.awaitSecondaryNodes();

    // The rollback node should replay the new operation.
    rollbackNode = rollbackTest.getSecondary();
    assert.eq(rollbackNode.getDB(dbName)[sourceCollName].find({_id: 0}).itcount(), 1);

    rollbackTest.stop();
}

testDowngrade("true");
testDowngrade("false");
})();
