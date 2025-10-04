/**
 * Initial sync runs in several phases - the first 3 are as follows:
 * 1) fetches the last oplog entry (op_start1) on the source;
 * 2) copies all non-local databases from the source; and
 * 3) fetches and applies operations from the source after op_start1.
 *
 * This test updates and deletes a document on the source between phases 1 and 2 in a prepared
 * transaction. The secondary will fail to apply the update operation in phase 3 but initial sync
 * completes nevertheless.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    finishAndValidate,
    insertUpdateRemoveLarge,
    reInitiateSetWithSecondary,
    turnOffHangBeforeCopyingDatabasesFailPoint,
    updateRemove,
} from "jstests/replsets/libs/initial_sync_update_missing_doc.js";

function doTest(doTransactionWork, numDocuments) {
    const replSet = new ReplSetTest({nodes: 1});

    replSet.startSet();
    replSet.initiate();
    const primary = replSet.getPrimary();
    const dbName = "test";
    const collectionName = jsTestName();
    let coll = primary.getDB(dbName).getCollection(collectionName);
    assert.commandWorked(coll.insert({_id: 0, x: 1}));
    assert.commandWorked(coll.insert({_id: 1, x: 1}));

    // Add a secondary node with priority: 0 and votes: 0 so that we prevent elections while
    // it is syncing from the primary.
    const secondaryConfig = {rsConfig: {votes: 0, priority: 0}};
    const secondary = reInitiateSetWithSecondary(replSet, secondaryConfig);

    const session = primary.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collectionName);

    session.startTransaction();
    doTransactionWork(sessionColl, {_id: 0});
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
    assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

    // This transaction is eventually aborted, so this document should exist on the secondary
    // after initial sync.
    session.startTransaction();
    doTransactionWork(sessionColl, {_id: 1});
    PrepareHelpers.prepareTransaction(session);
    assert.commandWorked(session.abortTransaction_forTesting());

    turnOffHangBeforeCopyingDatabasesFailPoint(secondary);

    // Since we aborted the second transaction, we expect this collection to still exist after
    // initial sync.
    finishAndValidate(replSet, collectionName, numDocuments);

    // Make sure the secondary has the correct documents after syncing from the primary. The
    // second document was deleted in the prepared transaction that was aborted. Therefore, it
    // should have been properly replication.
    coll = secondary.getDB(dbName).getCollection(collectionName);
    assert.docEq(null, coll.findOne({_id: 0}), "document on secondary matches primary");
    assert.docEq({_id: 1, x: 1}, coll.findOne({_id: 1}), "document on secondary matches primary");

    replSet.stopSet();
}

jsTestLog("Testing with prepared transaction");
// Passing in a function to update and remove document on primary in a prepared transaction
// between phrase 1 and 2. Once the secondary receives the commit for the transaction, the
// secondary should apply each operation separately (one update, and one delete) during initial
// sync.
doTest(updateRemove, 1 /* numDocuments after initial sync */);

jsTestLog("Testing with large prepared transaction");
// Passing in a function to insert, update and remove large documents on primary in a large
// prepared transaction. Once the secondary receives the commit for the transaction, the
// secondary should apply each operation separately (one insert, one update, and one delete)
// during initial sync.
doTest(insertUpdateRemoveLarge, 2 /* numDocuments after initial sync */);
