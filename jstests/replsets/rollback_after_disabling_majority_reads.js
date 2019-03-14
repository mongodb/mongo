/**
 * This test demonstrates that a rollback immediately after disabling majority reads succeeds.
 * @tags: [requires_persistence]
 */
(function() {
    "use strict";

    load("jstests/replsets/libs/rollback_test.js");

    TestData.rollbackShutdowns = true;
    const name = "rollback_after_disabling_majority_reads";
    const dbName = "test";
    const collName = "coll";

    jsTest.log("Set up a Rollback Test with enableMajorityReadConcern=true");
    const replTest = new ReplSetTest(
        {name, nodes: 3, useBridge: true, nodeOptions: {enableMajorityReadConcern: "true"}});
    replTest.startSet();
    let config = replTest.getReplSetConfig();
    config.members[2].arbiterOnly = true;
    replTest.initiate(config);
    const rollbackTest = new RollbackTest(name, replTest);

    const rollbackNode = rollbackTest.transitionToRollbackOperations();
    assert.commandWorked(rollbackNode.getDB(dbName)[collName].insert({_id: "rollback op"}));

    // Change primaries without initiating a rollback.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    assert.neq(rollbackTest.getPrimary(), rollbackNode);

    jsTest.log("Restart the rollback node with enableMajorityReadConcern=false");
    assert.eq(replTest.nodes[0], rollbackNode);
    rollbackTest.restartNode(0, 15, {enableMajorityReadConcern: "false"});
    // Make sure the primary has not changed.
    assert.neq(rollbackTest.getPrimary(), rollbackNode);

    // Initiate the rollback.
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    assert.commandWorked(rollbackTest.getPrimary().getDB(dbName)[collName].insert(
        {_id: "steady state op"}, {writeConcern: {w: "majority"}}));

    assert.eq(0, rollbackNode.getDB(dbName)[collName].find({_id: "rollback op"}).itcount());
    assert.eq(1, rollbackNode.getDB(dbName)[collName].find({_id: "steady state op"}).itcount());

    rollbackTest.stop();
}());