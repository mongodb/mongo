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
    // This test uses a smaller cache size than the other wt_cache_full.js tests because it
    // has to work with the hard-coded 300 MB refetch limit in the pre-4.0 rollback
    // implementation.
    wiredTigerCacheSizeGB: 0.5,
};
const rst = new ReplSetTest({
    nodes: 3,
    nodeOptions: nodeOptions,
    useBridge: true,
});

rst.startSet();
let config = rst.getReplSetConfig();
config.members[2].priority = 0;
config.settings = {
    chainingAllowed: false
};
rst.initiate(config);

// Prior to 4.0, rollback imposed a 300 MB limit on the total size of documents to refetch from
// the sync source. Therefore, we select values for numDocs and minDocSizeMB, while accounting
// for some small per-document overhead, such that we are able to stay under this 300 MB limit.
// This test uses single updates, rather than the multiple updates in the other wt_cache_full.js
// tests because the refetching logic in the pre-4.0 algorithm depends on which documents were
// modified, not on the number of modifications to each document.
// This test has been observed to hang under some non-standard build platforms so we are
// giving ourselves a slightly larger allowance of 5 documents from the theoretical maximum
// of documents calculated from the rollback size limit.
// Using a numDocs value of (maxDocs - 5) is sufficiently large enough to reproduce the memory
// pressure issue in 3.6.5 but small enough for this test to perform uniformly across most of
// the platforms in our continuous integration system.
const rollbackSizeLimitMB = 300;
const minDocSizeMB = 10;
const largeString = 'x'.repeat(minDocSizeMB * 1024 * 1024);
// TODO(SERVER-39774): Increase numDocs to Math.floor(rollbackSizeLimitMB / minDocSizeMB).
const numDocs = 1;

// Operations that will be present on both nodes, before the common point.
const collName = 'test.t';
let CommonOps = (node) => {
    const coll = node.getCollection(collName);
    jsTestLog('Inserting ' + numDocs + ' documents of ' + minDocSizeMB + ' MB each into ' +
              collName + '.');
    for (let i = 0; i < numDocs; ++i) {
        assert.writeOK(
            coll.save({_id: i, a: 0, x: largeString},
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
