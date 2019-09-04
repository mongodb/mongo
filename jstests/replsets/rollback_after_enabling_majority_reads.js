/**
 * This test documents the behavior that rolling back immediately after upgrading
 * enableMajorityReadConcern to true can fassert. If this happens, the user can restart the server
 * with enableMajorityReadConcern=false to complete the rollback, then upgrade again to
 * enableMajorityReadConcern=true.
 * Rollback after restarting with enableMajorityReadConcern=true succeeds if the stable timestamp
 * has been set, i.e. an operation has been majority committed.
 * @tags: [requires_persistence, requires_wiredtiger]
 */
(function() {
    "use strict";

    load("jstests/replsets/libs/rollback_test.js");

    TestData.rollbackShutdowns = true;
    const name = "rollback_after_enabling_majority_reads";
    const dbName = "test";
    const collName = "coll";

    jsTest.log("Set up a Rollback Test with enableMajorityReadConcern=false");
    const replTest = new ReplSetTest(
        {name, nodes: 3, useBridge: true, nodeOptions: {enableMajorityReadConcern: "false"}});
    replTest.startSet();
    let config = replTest.getReplSetConfig();
    config.members[2].arbiterOnly = true;
    replTest.initiate(config);
    let rollbackTest = new RollbackTest(name, replTest);

    let rollbackNode = rollbackTest.transitionToRollbackOperations();
    assert.commandWorked(rollbackNode.getDB(dbName)[collName].insert({_id: "rollback op"}));

    // Change primaries without initiating a rollback.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    assert.neq(rollbackTest.getPrimary(), rollbackNode);

    jsTest.log("Restart the rollback node with enableMajorityReadConcern=true");
    assert.eq(replTest.nodes[0], rollbackNode);
    rollbackTest.restartNode(0, 15, {enableMajorityReadConcern: "true"});
    // Make sure the primary has not changed.
    assert.neq(rollbackTest.getPrimary(), rollbackNode);

    // The rollback crashes because we have not yet set a stable timestamp.
    jsTest.log("Attempt to roll back. This will fassert.");
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    assert.soon(() => {
        return rawMongoProgramOutput().indexOf("Fatal assertion 50666") !== -1;
    });

    jsTest.log(
        "Restart the rollback node with enableMajorityReadConcern=false. Now the rollback can succeed.");
    const allowedExitCode = 14;
    rollbackTest.restartNode(0, 15, {enableMajorityReadConcern: "false"}, allowedExitCode);

    // Fix counts for "local.startup_log", since they are corrupted by this rollback.
    // transitionToSteadyStateOperations() checks collection counts.
    assert.commandWorked(rollbackNode.getDB("local").runCommand({validate: "startup_log"}));
    rollbackTest.transitionToSteadyStateOperations();

    assert.commandWorked(rollbackTest.getPrimary().getDB(dbName)[collName].insert(
        {_id: "steady state op"}, {writeConcern: {w: "majority"}}));

    assert.eq(0, rollbackNode.getDB(dbName)[collName].find({_id: "rollback op"}).itcount());
    assert.eq(1, rollbackNode.getDB(dbName)[collName].find({_id: "steady state op"}).itcount());

    jsTest.log(
        "Test that rollback succeeds with enableMajorityReadConcern=true once we have set a stable timestamp.");
    rollbackTest.restartNode(0, 15, {enableMajorityReadConcern: "true"});

    // Make sure node 0 is the primary.
    let node = replTest.nodes[0];
    jsTestLog("Waiting for node " + node.host + " to become primary.");
    replTest.awaitNodesAgreeOnPrimary();
    assert.commandWorked(node.adminCommand({replSetStepUp: 1}));
    replTest.waitForState(node, ReplSetTest.State.PRIMARY);
    assert.eq(replTest.getPrimary(), node, node.host + " was not primary after step up.");

    assert.commandWorked(replTest.getPrimary().getDB(dbName)[collName].insert(
        {_id: "advance stable timestamp"}, {writeConcern: {w: "majority"}}));
    replTest.awaitLastOpCommitted();

    // Create a new RollbackTest fixture to execute the final rollback. This will guarantee that the
    // final rollback occurs on the current primary, which should be node 0.
    jsTestLog("Creating a new RollbackTest fixture to execute a final rollback.");
    rollbackTest = new RollbackTest(name, replTest);
    rollbackNode = rollbackTest.transitionToRollbackOperations();
    assert.eq(replTest.nodes[0], rollbackNode);
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    assert.commandWorked(rollbackTest.getPrimary().getDB(dbName)[collName].insert(
        {_id: "steady state op 2"}, {writeConcern: {w: "majority"}}));

    assert.eq(
        1, rollbackNode.getDB(dbName)[collName].find({_id: "advance stable timestamp"}).itcount());
    assert.eq(1, rollbackNode.getDB(dbName)[collName].find({_id: "steady state op 2"}).itcount());

    rollbackTest.stop();
}());