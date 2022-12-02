/**
 * Test to ensure that a 'renameCollection' operation that occurs on the sync source past the
 * rollback common point for a collection that also exists on the rollback node will not cause data
 * corruption on the rollback node.
 */

(function() {
'use strict';

load("jstests/replsets/libs/rollback_test.js");

let dbName = "rollback_rename_collection_on_sync_source";
let otherDbName = "rollback_rename_collection_on_sync_source_other";
let sourceCollName = "sourceColl";
let destCollName = "destColl";
let sourceNs = dbName + '.' + sourceCollName;
let destNs = dbName + '.' + destCollName;
let otherDestNs = otherDbName + '.' + destCollName;

let doc1 = {x: 1};
let doc2 = {x: 2};

let CommonOps = (node) => {
    // Insert a document that will exist on the sync source and rollback node.
    assert.commandWorked(node.getDB(dbName)[sourceCollName].insert(doc1));
};

let RollbackOps = (node) => {
    // Delete the document on rollback node so it will be refetched from sync source.
    assert.commandWorked(node.getDB(dbName)[sourceCollName].remove(doc1));
};

let SyncSourceOps = (node, withinDB) => {
    // Rename the original collection on the sync source.
    let destCollection = (withinDB) ? destNs : otherDestNs;
    assert.commandWorked(
        node.getDB(dbName).adminCommand({renameCollection: sourceNs, to: destCollection}));
    assert.commandWorked(node.getDB(dbName)[destCollection].insert(doc2));
};

// 1. Rename to collection within same database.
// Set up Rollback Test.
let rollbackTest = new RollbackTest();
CommonOps(rollbackTest.getPrimary());

let rollbackNode = rollbackTest.transitionToRollbackOperations();
RollbackOps(rollbackNode);

let syncSourceNode = rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
SyncSourceOps(syncSourceNode, /*withinDB=*/true);

// Wait for rollback to finish.
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// Check the replica set.
rollbackTest.stop();

// 2. Rename to collection in another database.
// Set up Rollback Test.
let rollbackTestAcrossDBs = new RollbackTest();
CommonOps(rollbackTestAcrossDBs.getPrimary());

let rollbackNodeAcrossDBs = rollbackTestAcrossDBs.transitionToRollbackOperations();
RollbackOps(rollbackNodeAcrossDBs);

let syncSourceNodeAcrossDBs =
    rollbackTestAcrossDBs.transitionToSyncSourceOperationsBeforeRollback();
SyncSourceOps(syncSourceNodeAcrossDBs, /*withinDB=*/false);

// Wait for rollback to finish.
rollbackTestAcrossDBs.transitionToSyncSourceOperationsDuringRollback();
rollbackTestAcrossDBs.transitionToSteadyStateOperations();

// Check the replica set.
rollbackTestAcrossDBs.stop();
}());
