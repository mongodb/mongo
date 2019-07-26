/**
 * Tests that a collection drop can be rolled back.
 * @tags: [requires_replication, requires_wiredtiger]
 */
(function() {
'use strict';

load('jstests/replsets/libs/rollback_test.js');

// Returns list of collections in database, including pending drops.
// Assumes all collections fit in first batch of results.
function listCollections(database) {
    return assert
        .commandWorked(database.runCommand({listCollections: 1, includePendingDrops: true}))
        .cursor.firstBatch;
}

// Operations that will be present on both nodes, before the common point.
const collName = 'test.t';
const renameTargetCollName = 'test.x';
const noOpsToRollbackCollName = 'test.k';
let CommonOps = (node) => {
    const coll = node.getCollection(collName);
    const mydb = coll.getDB();
    assert.commandWorked(mydb.createCollection(coll.getName()));
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.insert({_id: 0, a: 0}));

    // Replicate a drop.
    const replicatedDropCollName = 'w';
    const collToDrop = mydb.getCollection(replicatedDropCollName);
    assert.commandWorked(mydb.createCollection(collToDrop.getName()));
    assert(collToDrop.drop());

    // This collection will be dropped during a rename.
    const renameTargetColl = node.getCollection(renameTargetCollName);
    assert.commandWorked(mydb.createCollection(renameTargetColl.getName()));
    assert.commandWorked(renameTargetColl.createIndex({b: 1}));
    assert.commandWorked(renameTargetColl.insert({_id: 8, b: 8}));
    assert.commandWorked(renameTargetColl.insert({_id: 9, b: 9}));

    // This collection will be dropped without any CRUD ops to rollback.
    const noOpsToRollbackColl = node.getCollection(noOpsToRollbackCollName);
    assert.commandWorked(mydb.createCollection(noOpsToRollbackColl.getName()));
    assert.commandWorked(noOpsToRollbackColl.createIndex({c: 1}));
    assert.commandWorked(noOpsToRollbackColl.insert({_id: 20, c: 20}));
    assert.commandWorked(noOpsToRollbackColl.insert({_id: 21, c: 21}));
};

// Operations that will be performed on the rollback node past the common point.
let RollbackOps = (node) => {
    const coll = node.getCollection(collName);

    // Rollback algorithm may refer to dropped collection if it has to undo an insert.
    assert.commandWorked(coll.insert({_id: 1, a: 1}));

    const mydb = coll.getDB();
    const collectionsBeforeDrop = listCollections(mydb);
    assert(coll.drop());
    const collectionsAfterDrop = listCollections(mydb);
    const supportsPendingDrops = mydb.serverStatus().storageEngine.supportsPendingDrops;
    jsTestLog('supportsPendingDrops = ' + supportsPendingDrops);
    if (!supportsPendingDrops) {
        assert.eq(collectionsAfterDrop.length,
                  collectionsBeforeDrop.length,
                  'listCollections did not report the same number of collections in database ' +
                      mydb.getName() + ' after dropping collection ' + coll.getFullName() +
                      '. Before: ' + tojson(collectionsBeforeDrop) +
                      '. After: ' + tojson(collectionsAfterDrop));
    } else {
        assert.lt(collectionsAfterDrop.length,
                  collectionsBeforeDrop.length,
                  'listCollections did not report fewer collections in database ' + mydb.getName() +
                      ' after dropping collection ' + coll.getFullName() + '. Before: ' +
                      tojson(collectionsBeforeDrop) + '. After: ' + tojson(collectionsAfterDrop));
        assert.gt(mydb.serverStatus().storageEngine.dropPendingIdents,
                  0,
                  'There is no drop pending ident in the storage engine.');
    }

    const renameTargetColl = node.getCollection(renameTargetCollName);
    assert.commandWorked(renameTargetColl.insert({_id: 10, b: 10}));
    assert.commandWorked(renameTargetColl.insert({_id: 11, b: 11}));
    const renameSourceColl = mydb.getCollection('z');
    assert.commandWorked(mydb.createCollection(renameSourceColl.getName()));
    assert.commandWorked(renameSourceColl.renameCollection(renameTargetColl.getName(), true));

    const noOpsToRollbackColl = node.getCollection(noOpsToRollbackCollName);
    assert(noOpsToRollbackColl.drop());

    // This collection will not exist after rollback.
    const tempColl = node.getCollection('test.a');
    assert.commandWorked(mydb.createCollection(tempColl.getName()));
    assert.commandWorked(tempColl.insert({_id: 100, y: 100}));
    assert(tempColl.drop());

    // restartCatalog should not remove drop-pending idents.
    assert.commandWorked(mydb.adminCommand({restartCatalog: 1}));
};

// Set up Rollback Test.
const rollbackTest = new RollbackTest();
CommonOps(rollbackTest.getPrimary());

const rollbackNode = rollbackTest.transitionToRollbackOperations();
RollbackOps(rollbackNode);

{
    // Check collection drop oplog entry.
    const replTest = rollbackTest.getTestFixture();
    const ops = replTest.dumpOplog(rollbackNode, {ns: 'test.$cmd', 'o.drop': 't'});
    assert.eq(1, ops.length);
    const op = ops[0];
    assert(op.hasOwnProperty('o2'), 'expected o2 field in drop oplog entry: ' + tojson(op));
    assert(op.o2.hasOwnProperty('numRecords'), 'expected count in drop oplog entry: ' + tojson(op));
    assert.eq(2, op.o2.numRecords, 'incorrect count in drop oplog entry: ' + tojson(op));
}

// Check collection rename oplog entry.
{
    const replTest = rollbackTest.getTestFixture();
    const ops = replTest.dumpOplog(
        rollbackNode, {ns: 'test.$cmd', 'o.renameCollection': 'test.z', 'o.to': 'test.x'});
    assert.eq(1, ops.length);
    const op = ops[0];
    assert(op.hasOwnProperty('o2'), 'expected o2 field in rename oplog entry: ' + tojson(op));
    assert(op.o2.hasOwnProperty('numRecords'),
           'expected count in rename oplog entry: ' + tojson(op));
    assert.eq(4, op.o2.numRecords, 'incorrect count in rename oplog entry: ' + tojson(op));
}

// Wait for rollback to finish.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// Check collection count.
const primary = rollbackTest.getPrimary();
const coll = primary.getCollection(collName);
assert.eq(1, coll.find().itcount());
assert.eq(1, coll.count());
const renameTargetColl = primary.getCollection(renameTargetCollName);
assert.eq(2, renameTargetColl.find().itcount());
assert.eq(2, renameTargetColl.count());
const noOpsToRollbackColl = primary.getCollection(noOpsToRollbackCollName);
assert.eq(2, noOpsToRollbackColl.find().itcount());
assert.eq(2, noOpsToRollbackColl.count());

rollbackTest.stop();
})();
