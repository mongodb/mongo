/**
 * Asserts that inserting a document like `{a:[{"0":1}]}` on the
 * primary doesn't cause initial-sync to fail if there was index
 * on `a.0` at the beginning of initial-sync but the `dropIndex`
 * hasn't yet been applied on the secondary prior to cloning the
 * insert oplog entry.
 */

(function() {
    "use strict";

    load("jstests/libs/check_log.js");

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

    // Insert the initial document set.
    for (let i = 0; i < initialDocs; ++i) {
        primaryColl.insertOne({_id: i, a: i});
    }

    // Add a secondary.
    const secondary = rst.add({
        setParameter: {"numInitialSyncAttempts": 1, 'collectionClonerBatchSize': clonerBatchSize}
    });
    secondary.setSlaveOk();
    const secondaryColl = secondary.getDB(dbName).getCollection(collectionName);

    // We set the collectionClonerBatchSize low above, so we will definitely hit
    // this fail-point and hang after the first batch is applied. While the
    // secondary is hung we clone the problematic document.
    secondary.adminCommand({
        configureFailPoint: "initialSyncHangBeforeCopyingDatabases",
        mode: "alwaysOn",
        data: {nss: secondaryColl.getFullName()}
    });
    rst.reInitiate();
    checkLog.contains(secondary, "initialSyncHangBeforeCopyingDatabases fail point enabled");

    // Insert and delete the problematic document and then create the problematic index.
    // The collection-cloner will resume when the failpoint is turned off.
    primaryColl.insertOne({_id: 200, a: [{"0": 1}]});
    primaryColl.deleteOne({_id: 200});
    primaryColl.ensureIndex({"a.0": 1});

    // Resume initial sync. The "bad" document will be applied; the dropIndex will
    // be applied later.
    secondary.adminCommand(
        {configureFailPoint: "initialSyncHangBeforeCopyingDatabases", mode: "off"});

    // Wait for initial sync to finish.
    rst.awaitSecondaryNodes();
    rst.stopSet();
})();
