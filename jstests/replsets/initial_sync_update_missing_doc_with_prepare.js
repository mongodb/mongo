/**
 * Initial sync runs in several phases - the first 3 are as follows:
 * 1) fetches the last oplog entry (op_start1) on the source;
 * 2) copies all non-local databases from the source; and
 * 3) fetches and applies operations from the source after op_start1.
 *
 * This test updates and deletes a document on the source between phases 1 and 2 in a prepared
 * transaction. The secondary will initially fail to apply the update operation in phase 3 and
 * subsequently have to attempt to check the source for a new copy of the document. The absence of
 * the document on the source indicates that the source is free to ignore the failed update
 * operation.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    load("jstests/core/txns/libs/prepare_helpers.js");
    load("jstests/replsets/libs/initial_sync_update_missing_doc.js");
    load("jstests/libs/check_log.js");

    const name = 'initial_sync_update_missing_doc_with_prepare';
    const replSet = new ReplSetTest({
        name: name,
        nodes: 1,
    });

    replSet.startSet();
    replSet.initiate();
    const primary = replSet.getPrimary();
    const dbName = 'test';

    var coll = primary.getDB(dbName).getCollection(name);
    assert.commandWorked(coll.insert({_id: 0, x: 1}));
    assert.commandWorked(coll.insert({_id: 1, x: 1}));

    // Add a secondary node with priority: 0 and votes: 0 so that we prevent elections while
    // it is syncing from the primary.
    const secondaryConfig = {rsConfig: {votes: 0, priority: 0}};
    const secondary = reInitiateSetWithSecondary(replSet, secondaryConfig);

    const session = primary.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(name);
    // Update and remove document on primary in a prepared transaction. Once we receive the
    // commit for this transaction, we should apply each operation separately (one update, and
    // one delete).
    session.startTransaction();
    assert.commandWorked(sessionColl.update({_id: 0}, {x: 2}, {upsert: false}));
    assert.commandWorked(sessionColl.remove({_id: 0}, {justOne: true}));
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
    assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

    // This transaction is eventually aborted, so this document should exist on the secondary
    // after initial sync.
    session.startTransaction();
    assert.commandWorked(sessionColl.update({_id: 1}, {x: 2}, {upsert: false}));
    assert.commandWorked(sessionColl.remove({_id: 1}, {justOne: true}));
    PrepareHelpers.prepareTransaction(session);
    assert.commandWorked(session.abortTransaction_forTesting());

    turnOffHangBeforeCopyingDatabasesFailPoint(secondary);

    var res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
    assert.eq(res.initialSyncStatus.fetchedMissingDocs, 0);
    var firstOplogEnd = res.initialSyncStatus.initialSyncOplogEnd;

    turnOffHangBeforeGettingMissingDocFailPoint(primary, secondary, name, 0 /* numInserted */);

    // Since we aborted the second transaction, we expect this collection to still exist after
    // initial sync.
    finishAndValidate(replSet, name, firstOplogEnd, 0 /* numInserted */, 1 /* numCollections */);

    // Make sure the secondary has the correct documents after syncing from the primary. The
    // second document was deleted in the prepared transaction that was aborted. Therefore, it
    // should have been properly replication.
    coll = secondary.getDB(dbName).getCollection(name);
    assert.docEq(null, coll.findOne({_id: 0}), 'document on secondary matches primary');
    assert.docEq({_id: 1, x: 1}, coll.findOne({_id: 1}), 'document on secondary matches primary');

    replSet.stopSet();

})();