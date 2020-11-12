/**
 * Asserts that inserting a document like `{a:[{"0":1}]}` on the
 * primary doesn't cause initial-sync to fail if there was index
 * on `a.0` at the beginning of initial-sync but the `dropIndex`
 * hasn't yet been applied on the secondary prior to cloning the
 * insert oplog entry.
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const dbName = 'test';
const collectionName = 'coll';

// How many documents to insert on the primary prior to
// starting initial-sync.
const initialDocs = 10;
// Batch-size used for cloning.
// Used as a fail-point parameter as detailed below.
const clonerBatchSize = 1;

// Setting initialDocs larger than clonerBatchSize causes
// the fail-point to be hit because we fetch
// multiple batches in the InitialSyncer.

// Start one-node repl-set.
const rst = new ReplSetTest({name: "apply_ops_ambiguous_index", nodes: 1});
rst.startSet();
rst.initiate();
const primaryColl = rst.getPrimary().getDB(dbName).getCollection(collectionName);

// Create the index.
primaryColl.ensureIndex({"a.0": 1});

// Insert the initial document set.
for (let i = 0; i < initialDocs; ++i) {
    primaryColl.insertOne({_id: i, a: i});
}

// Add a secondary.
const secondary = rst.add({
    rsConfig: {votes: 0, priority: 0},
    setParameter: {"numInitialSyncAttempts": 1, 'collectionClonerBatchSize': clonerBatchSize}
});
secondary.setSecondaryOk();
const secondaryColl = secondary.getDB(dbName).getCollection(collectionName);

// We set the collectionClonerBatchSize low above, so we will definitely hit
// this fail-point and hang after the first batch is applied. While the
// secondary is hung we clone the problematic document.
const failPoint = configureFailPoint(secondary,
                                     "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
                                     {nss: secondaryColl.getFullName()});
rst.reInitiate();
failPoint.wait();

// Primary no longer has the problematic index so the insert succeeds on the primary.
// The collection-cloner will resume when the failpoint is turned off.
primaryColl.dropIndex({"a.0": 1});
primaryColl.insertOne({_id: 200, a: [{"0": 1}]});

// Resume initial sync. The "bad" document will be applied; the dropIndex will
// be applied later.
failPoint.off();

// Wait for initial sync to finish.
rst.awaitSecondaryNodes();

// Check document count on secondary.
// initialDocs from the initial `for` loop and the 1 "bad" document.
assert.eq(initialDocs + 1, secondaryColl.find().itcount());

rst.stopSet();
})();
