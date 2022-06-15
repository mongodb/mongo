// When run with --majorityReadConcern=off, this test reproduces the bug described in SERVER-38925,
// where rolling back a delete followed by a restart produces documents with duplicate _id.
// @tags: [requires_persistence]
(function() {
"use strict";

load("jstests/replsets/libs/rollback_test.js");

TestData.rollbackShutdowns = true;
TestData.allowUncleanShutdowns = true;
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
RollbackOps(rollbackNode);

// Wait for rollback to finish.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// Kill and restart the node that rolled back.
rollbackTest.restartNode(0, 9);

// Check the replica set.
rollbackTest.stop();
}());
