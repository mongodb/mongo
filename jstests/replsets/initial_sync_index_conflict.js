/**
 * Asserts that applying a single-phase createIndexes oplog entry doesn't cause initial-sync to fail
 * if there is an ongoing two-phase index build started by the cloner with the same index specs.
 * This may happen if the index was first created on the primary when the collection was empty, then
 * subsequently dropped and recreated after the collection has documents.
 */

(function() {
'use strict';

load('jstests/libs/fail_point_util.js');
load('jstests/noPassthrough/libs/index_build.js');

const dbName = 'test';
const collectionName = 'coll';

// Start one-node repl-set.
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collectionName);

// Add a secondary.
const secondary = rst.add({
    rsConfig: {votes: 0, priority: 0},
    setParameter: {numInitialSyncAttempts: 1},
});

// While the secondary is hung, we create the same index multiple times to
// reproduce the interaction between single and two phase index builds on the
// same index.
const failPoint = configureFailPoint(secondary, 'initialSyncHangBeforeCopyingDatabases');
rst.reInitiate();
failPoint.wait();

// Create the index using the empty collection fast path. The index build should be replicated
// in a single createIndexes oplog entry.
assert.commandWorked(primaryColl.createIndex({a: 1}));

assert.commandWorked(primaryColl.insert({_id: 0, a: 0}));

// Start a two-phase index build using the same spec when the collection has documents.
// Use a failpoint to keep the index build from finishing when we resume initial sync on
// the secondary.
assert.commandWorked(primaryColl.dropIndex({a: 1}));
IndexBuildTest.pauseIndexBuilds(primary);
IndexBuildTest.pauseIndexBuilds(secondary);
const createIdx = IndexBuildTest.startIndexBuild(primary, primaryColl.getFullName(), {a: 1});
IndexBuildTest.waitForIndexBuildToScanCollection(primaryDB, primaryColl.getName(), 'a_1');
try {
    // Resume initial sync. The createIndexes oplog entry will be applied.
    failPoint.off();

    // Wait for initial sync to finish.
    rst.awaitSecondaryNodes();
} finally {
    IndexBuildTest.resumeIndexBuilds(secondary);
    IndexBuildTest.resumeIndexBuilds(primary);
    createIdx();
}

IndexBuildTest.assertIndexes(primaryColl, 2, ['_id_', 'a_1']);

rst.stopSet();
})();
