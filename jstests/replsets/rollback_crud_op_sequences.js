/*
 * Basic test of a succesful replica set rollback for CRUD operations.
 */
load('jstests/replsets/libs/rollback_test.js');
load("jstests/replsets/rslib.js");

(function() {
"use strict";

// Helper function for verifying contents at the end of the test.
const checkFinalResults = function(db) {
    assert.eq(0, db.bar.count({q: 70}));
    assert.eq(2, db.bar.count({q: 40}));
    assert.eq(3, db.bar.count({a: "foo"}));
    assert.eq(6, db.bar.count({q: {$gt: -1}}));
    assert.eq(1, db.bar.count({txt: "foo"}));
    assert.eq(33, db.bar.findOne({q: 0})["y"]);
    assert.eq(1, db.kap.find().itcount());
    assert.eq(0, db.kap2.find().itcount());
};

const rollbackTest = new RollbackTest();

const rollbackNode = rollbackTest.getPrimary();
rollbackNode.setSecondaryOk();
const syncSource = rollbackTest.getSecondary();
syncSource.setSecondaryOk();

const rollbackNodeDB = rollbackNode.getDB("foo");
const syncSourceDB = syncSource.getDB("foo");

// Insert initial data for both nodes.
assert.commandWorked(rollbackNodeDB.bar.insert({q: 0}));
assert.commandWorked(rollbackNodeDB.bar.insert({q: 1, a: "foo"}));
assert.commandWorked(rollbackNodeDB.bar.insert({q: 2, a: "foo", x: 1}));
assert.commandWorked(rollbackNodeDB.bar.insert({q: 3, bb: 9, a: "foo"}));
assert.commandWorked(rollbackNodeDB.bar.insert({q: 40, a: 1}));
assert.commandWorked(rollbackNodeDB.bar.insert({q: 40, a: 2}));
assert.commandWorked(rollbackNodeDB.bar.insert({q: 70, txt: 'willremove'}));
rollbackNodeDB.createCollection("kap", {capped: true, size: 5000});
assert.commandWorked(rollbackNodeDB.kap.insert({foo: 1}));
// Going back to empty on capped is a special case and must be tested.
rollbackNodeDB.createCollection("kap2", {capped: true, size: 5501});
rollbackTest.awaitReplication();

rollbackTest.transitionToRollbackOperations();

// These operations are only done on 'rollbackNode' and should eventually be rolled back.
assert.commandWorked(rollbackNodeDB.bar.insert({q: 4}));
assert.commandWorked(rollbackNodeDB.bar.update({q: 3}, {q: 3, rb: true}));
assert.commandWorked(rollbackNodeDB.bar.remove({q: 40}));  // multi remove test
assert.commandWorked(rollbackNodeDB.bar.update({q: 2}, {q: 39, rb: true}));
// Rolling back a delete will involve reinserting the item(s).
assert.commandWorked(rollbackNodeDB.bar.remove({q: 1}));
assert.commandWorked(rollbackNodeDB.bar.update({q: 0}, {$inc: {y: 1}}));
assert.commandWorked(rollbackNodeDB.kap.insert({foo: 2}));
assert.commandWorked(rollbackNodeDB.kap2.insert({foo: 2}));
// Create a collection (need to roll back the whole thing).
assert.commandWorked(rollbackNodeDB.newcoll.insert({a: true}));
// Create a new empty collection (need to roll back the whole thing).
rollbackNodeDB.createCollection("abc");

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

// Insert new data into syncSource so that rollbackNode enters rollback when it is reconnected.
// These operations should not be rolled back.
assert.gte(syncSourceDB.bar.find().itcount(), 1, "count check");
assert.commandWorked(syncSourceDB.bar.insert({txt: 'foo'}));
assert.commandWorked(syncSourceDB.bar.remove({q: 70}));
assert.commandWorked(syncSourceDB.bar.update({q: 0}, {$inc: {y: 33}}));

rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

rollbackTest.awaitReplication();
checkFinalResults(rollbackNodeDB);
checkFinalResults(syncSourceDB);

rollbackTest.stop();
}());
