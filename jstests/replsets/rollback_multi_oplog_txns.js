/**
 * Tests that an unprepared transaction can be rolled back.
 * @tags: [requires_replication, requires_wiredtiger]
 */
(function() {
    'use strict';

    load('jstests/libs/check_log.js');
    load('jstests/replsets/libs/rollback_test.js');

    // Operations that will be present on both nodes, before the common point.
    const dbName = 'test';
    const collName = 'test.t';
    const collNameShort = 't';
    let CommonOps = (node) => {
        const coll = node.getCollection(collName);
        const mydb = coll.getDB();
        assert.commandWorked(coll.insert({_id: 0}));
    };

    // Operations that will be performed on the rollback node past the common point.
    let RollbackOps = (node) => {
        const session = node.startSession();
        const sessionDB = session.getDatabase(dbName);
        const sessionColl = sessionDB.getCollection(collNameShort);
        session.startTransaction();
        assert.commandWorked(sessionColl.insert({_id: 1}));
        assert.commandWorked(sessionColl.insert({_id: 2}));
        assert.commandWorked(session.commitTransaction_forTesting());
        session.endSession();
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

    // Check logs and data directory to confirm that rollback wrote deleted documents to file.
    checkLog.contains(
        rollbackNode,
        'Preparing to write deleted documents to a rollback file for collection ' + collName);
    const replTest = rollbackTest.getTestFixture();
    const rollbackDir = replTest.getDbPath(rollbackNode) + '/rollback';
    assert(pathExists(rollbackDir), 'directory for rollback files does not exist: ' + rollbackDir);

    rollbackTest.stop();
})();
