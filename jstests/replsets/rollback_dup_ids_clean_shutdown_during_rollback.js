// When run with --majorityReadConcern=off, this test reproduces the bug described in SERVER-38925,
// where rolling back a delete followed by a restart produces documents with duplicate _id.
//
// In this test we also make sure that a clean shutdown during rollback does not overwrite the
// unstable checkpoints taken during the rollback process.
//
// @tags: [requires_persistence]
//
(function() {
"use strict";

load("jstests/replsets/libs/rollback_test.js");

TestData.rollbackShutdowns = true;
let dbName = "test";
let sourceCollName = "coll";

let doc1 = {_id: 1, x: "document_of_interest"};

let CommonOps = (node) => {
    // Insert a document that will exist on all nodes.
    assert.commandWorked(node.getDB(dbName)[sourceCollName].insert(doc1));
};

let RollbackOps = (node) => {
    // Delete the document on rollback node so it will be refetched from sync source.
    assert.commandWorked(node.getDB(dbName)[sourceCollName].remove({_id: 1}));
};

// Set up Rollback Test.
let rollbackTest = new RollbackTest();
CommonOps(rollbackTest.getPrimary());

let rollbackNode = rollbackTest.transitionToRollbackOperations();
// Have rollback hang after it has taken an unstable checkpoint but before it completes.
rollbackNode.adminCommand({configureFailPoint: 'bgSyncHangAfterRunRollback', mode: 'alwaysOn'});
RollbackOps(rollbackNode);

// Wait for rollback to finish.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();

jsTestLog("Waiting for rollback node to hit failpoint.");
checkLog.contains(rollbackNode, "bgSyncHangAfterRunRollback failpoint is set");

// Sending a shutdown signal to the node should cause us to break out of the hung failpoint, so we
// don't need to explicitly turn the failpoint off.
jsTestLog("Restarting rollback node with a clean shutdown.");
rollbackTest.restartNode(0, 15 /* SIGTERM */);

rollbackTest.transitionToSteadyStateOperations();

// Check the replica set.
rollbackTest.stop();
}());
