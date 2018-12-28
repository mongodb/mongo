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
    let CommonOps = (node) => {
        const coll = node.getCollection(collName);
        const mydb = coll.getDB();
        assert.commandWorked(mydb.createCollection(coll.getName()));
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.writeOK(coll.insert({_id: 0, a: 0}));

        // Replicate a drop.
        const replicatedDropCollName = 'w';
        const collToDrop = mydb.getCollection(replicatedDropCollName);
        assert.commandWorked(mydb.createCollection(collToDrop.getName()));
        assert(collToDrop.drop());
    };

    // Operations that will be performed on the rollback node past the common point.
    let RollbackOps = (node) => {
        const coll = node.getCollection(collName);

        // Rollback algorithm may refer to dropped collection if it has to undo an insert.
        assert.writeOK(coll.insert({_id: 1, a: 1}));

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
                          '. Before: ' + tojson(collectionsBeforeDrop) + '. After: ' +
                          tojson(collectionsAfterDrop));
        } else {
            assert.lt(collectionsAfterDrop.length,
                      collectionsBeforeDrop.length,
                      'listCollections did not report fewer collections in database ' +
                          mydb.getName() + ' after dropping collection ' + coll.getFullName() +
                          '. Before: ' + tojson(collectionsBeforeDrop) + '. After: ' +
                          tojson(collectionsAfterDrop));
        }

        // restartCatalog should not remove drop-pending idents.
        assert.commandWorked(mydb.adminCommand({restartCatalog: 1}));
    };

    // Set up Rollback Test.
    const rollbackTest = new RollbackTest();
    CommonOps(rollbackTest.getPrimary());

    const rollbackNode = rollbackTest.transitionToRollbackOperations();
    RollbackOps(rollbackNode);

    // Wait for rollback to finish.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    // Check collection count.
    const primary = rollbackTest.getPrimary();
    const coll = primary.getCollection(collName);
    assert.eq(1, coll.find().itcount());
    assert.eq(1, coll.count());

    rollbackTest.stop();
})();
