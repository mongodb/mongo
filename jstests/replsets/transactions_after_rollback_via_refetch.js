/**
 * Basic test that transactions are able to run against a node immediately after it has executed a
 * refetch based rollback of a few basic CRUD and DDL ops. Local writes done during the rollback
 * process are not timestamped, so we want to ensure that transactions can be started against a
 * valid snapshot post-rollback and read data correctly.
 *
 * @tags: [uses_transactions]
 */
(function() {
'use strict';

load("jstests/replsets/libs/rollback_test.js");

let name = "transactions_after_rollback_via_refetch";
let dbName = name;
let crudCollName = "crudColl";
let collToDropName = "collToDrop";

let CommonOps = (node) => {
    // Insert a couple of documents that will initially be present on all nodes.
    let crudColl = node.getDB(dbName)[crudCollName];
    assert.commandWorked(crudColl.insert({_id: 0}));
    assert.commandWorked(crudColl.insert({_id: 1}));

    // Create a collection so it can be dropped on the rollback node.
    node.getDB(dbName)[collToDropName].insert({_id: 0});
};

// We want to have the rollback node perform some inserts, updates, and deletes locally
// during the rollback process, so we can ensure that transactions will read correct data
// post-rollback, even though these writes will be un-timestamped.
let RollbackOps = (node) => {
    let crudColl = node.getDB(dbName)[crudCollName];
    // Roll back an update (causes refetch and local update).
    assert.commandWorked(crudColl.update({_id: 0}, {$set: {rollbackNode: 0}}));
    // Roll back a delete (causes refetch and local insert).
    assert.commandWorked(crudColl.remove({_id: 1}));
    // Roll back an insert (causes local delete).
    assert.commandWorked(crudColl.insert({_id: 2}));

    // Roll back a drop (re-creates the collection).
    node.getDB(dbName)[collToDropName].drop();
};

let SyncSourceOps = (node) => {
    let coll = node.getDB(dbName)[crudCollName];
    // Update these docs so the rollback node will refetch them.
    assert.commandWorked(coll.update({_id: 0}, {$set: {syncSource: 0}}));
    assert.commandWorked(coll.update({_id: 1}, {$set: {syncSource: 1}}));
};

// Set up a replica set for use in RollbackTest. We disable majority reads on all nodes so that
// they will use the "rollbackViaRefetch" algorithm.
let replTest = new ReplSetTest({
    name,
    nodes: 3,
    useBridge: true,
    settings: {chainingAllowed: false},
    nodeOptions: {enableMajorityReadConcern: "false"}
});
replTest.startSet();
let config = replTest.getReplSetConfig();
config.members[2].priority = 0;
replTest.initiate(config);

let rollbackTest = new RollbackTest(name, replTest);

CommonOps(rollbackTest.getPrimary());

let rollbackNode = rollbackTest.transitionToRollbackOperations();
RollbackOps(rollbackNode);

let syncSourceNode = rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
SyncSourceOps(syncSourceNode);

// Wait for rollback to finish.
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// Make the rollback node primary so we can run transactions against it.
rollbackTest.getTestFixture().stepUp(rollbackNode);

jsTestLog("Testing transactions against the node that just rolled back.");
const sessionOptions = {
    causalConsistency: false
};
let session = rollbackNode.getDB(dbName).getMongo().startSession(sessionOptions);
let sessionDb = session.getDatabase(dbName);
let sessionColl = sessionDb[crudCollName];

// Make sure we can do basic CRUD ops inside a transaction and read the data back correctly, pre
// and post-commit.
session.startTransaction();
// Make sure we read from the snapshot correctly.
assert.docEq(sessionColl.find().sort({_id: 1}).toArray(),
             [{_id: 0, syncSource: 0}, {_id: 1, syncSource: 1}]);
// Do some basic ops.
assert.commandWorked(sessionColl.update({_id: 0}, {$set: {inTxn: 1}}));
assert.commandWorked(sessionColl.remove({_id: 1}));
assert.commandWorked(sessionColl.insert({_id: 2}));
// Make sure we read the updated data correctly.
assert.docEq(sessionColl.find().sort({_id: 1}).toArray(),
             [{_id: 0, syncSource: 0, inTxn: 1}, {_id: 2}]);
assert.commandWorked(session.commitTransaction_forTesting());

// Make sure data is visible after commit.
assert.docEq(sessionColl.find().sort({_id: 1}).toArray(),
             [{_id: 0, syncSource: 0, inTxn: 1}, {_id: 2}]);

// Run a transaction that touches the collection that was re-created during rollback.
sessionColl = sessionDb[collToDropName];
session.startTransaction();
assert.docEq(sessionColl.find().sort({_id: 1}).toArray(), [{_id: 0}]);
assert.commandWorked(sessionColl.update({_id: 0}, {$set: {inTxn: 1}}));
assert.commandWorked(session.commitTransaction_forTesting());

// Make sure data is visible after commit.
assert.docEq(sessionColl.find().sort({_id: 1}).toArray(), [{_id: 0, inTxn: 1}]);

// Check the replica set.
rollbackTest.stop();
}());
