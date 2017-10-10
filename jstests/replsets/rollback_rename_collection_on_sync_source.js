/**
 * Test to ensure that a 'renameCollection' operation that occurs on the sync source past the
 * rollback common point for a collection that also exists on the rollback node will not cause data
 * corruption on the rollback node.
 */

(function() {
    'use strict';

    load("jstests/replsets/libs/rollback_test.js");

    let dbName = "rollback_rename_collection_on_sync_source";
    let sourceCollName = "sourceColl";
    let destCollName = "destColl";

    let doc1 = {x: 1};
    let doc2 = {x: 2};

    let CommonOps = (node) => {
        // Insert a document that will exist on the sync source and rollback node.
        assert.writeOK(node.getDB(dbName)[sourceCollName].insert(doc1));
    };

    let RollbackOps = (node) => {
        // Delete the document on rollback node so it will be refetched from sync source.
        assert.writeOK(node.getDB(dbName)[sourceCollName].remove(doc1));
    };

    let SyncSourceOps = (node) => {
        // Rename the original collection on the sync source.
        assert.commandWorked(node.getDB(dbName)[sourceCollName].renameCollection(destCollName));
        assert.writeOK(node.getDB(dbName)[destCollName].insert(doc2));
    };

    // Set up Rollback Test.
    let rollbackTest = new RollbackTest();
    CommonOps(rollbackTest.getPrimary());

    let rollbackNode = rollbackTest.transitionToRollbackOperations();
    RollbackOps(rollbackNode);

    let syncSourceNode = rollbackTest.transitionToSyncSourceOperations();
    SyncSourceOps(syncSourceNode);

    // Wait for rollback to finish.
    rollbackTest.transitionToSteadyStateOperations({waitForRollback: true});

    // Check the replica set.
    rollbackTest.stop();

}());