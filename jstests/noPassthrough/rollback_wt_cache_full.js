/**
 * Tests that rollback succeeds even when the WiredTiger cache is filled.
 * See WT-3698.
 * @tags: [requires_replication, requires_wiredtiger]
 */
(function() {
    'use strict';

    load('jstests/replsets/libs/rollback_test.js');

    // Use constrained cache size for both data bearing nodes so that it doesn't matter which node
    // RollbackTest selects as the rollback node.
    const nodeOptions = {
        // Don't log slow operations.
        slowms: 30000,
        // Constrain the storage engine cache size to make it easier to fill it up with unflushed
        // modifications.
        wiredTigerCacheSizeGB: 1,
    };
    const rst = new ReplSetTest({
        nodes: [nodeOptions, nodeOptions, {arbiter: true}],
        useBridge: true,
    });
    const nodes = rst.startSet();
    rst.initiate();

    // Prior to 4.0, rollback imposed a 300 MB limit on the total size of documents to refetch from
    // the sync source. Therefore, we select values for numDocs and minDocSizeMB, while accounting
    // for some small per-document overhead, such that we are able to stay under this 300 MB limit.
    // This test uses single updates, rather than the multiple updates in the other wt_cache_full.js
    // tests because the refetching logic in the pre-4.0 algorithm depends on which documents were
    // modified, not on the number of modifications to each document.
    const rollbackSizeLimitMB = 300;
    const minDocSizeMB = 10;
    const largeString = 'x'.repeat(minDocSizeMB * 1024 * 1024);
    const numDocs = Math.floor(rollbackSizeLimitMB / minDocSizeMB) - 1;

    // Operations that will be present on both nodes, before the common point.
    const collName = 'test.t';
    let CommonOps = (node) => {
        const coll = node.getCollection(collName);
        jsTestLog('Inserting ' + numDocs + ' documents of ' + minDocSizeMB + ' MB each into ' +
                  collName + '.');
        for (let i = 0; i < numDocs; ++i) {
            assert.writeOK(coll.save(
                {_id: i, a: 0, x: largeString},
                {writeConcern: {w: 'majority', wtimeout: ReplSetTest.kDefaultTimeoutMS}}));
        }
        assert.eq(numDocs, coll.find().itcount());
    };

    // Operations that will be performed on the rollback node past the common point.
    let RollbackOps = (node) => {
        const coll = node.getCollection(collName);
        jsTestLog('Updating ' + numDocs +
                  ' documents on the primary. These updates will be rolled back.');
        for (let i = 0; i < numDocs; ++i) {
            assert.writeOK(coll.update({_id: i}, {$inc: {a: 1}}));
        }
    };

    // Set up Rollback Test.
    const rollbackTest = new RollbackTest(rst.name, rst);
    CommonOps(rollbackTest.getPrimary());

    const rollbackNode = rollbackTest.transitionToRollbackOperations();
    RollbackOps(rollbackNode);

    // Wait for rollback to finish.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    rollbackTest.stop();
})();
