/**
 * Verifies that a write that sets the on-disk multikey flag does not generate prepare conflicts
 * that would lead to a deadlock during secondary oplog application.
 *
 * This is a regression test for SERVER-41766.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");

    const replTest = new ReplSetTest({name: 'multikey_write_avoids_prepare_conflict', nodes: 2});
    replTest.startSet();
    replTest.initiate();

    const dbName = "test";
    const collName = "coll";

    const primary = replTest.getPrimary();
    const secondary = replTest.getSecondary();
    const primaryColl = primary.getDB(dbName)[collName];

    jsTestLog("Creating a collection and an index on the primary, with spec {x:1}.");
    assert.commandWorked(primaryColl.createIndex({x: 1}));
    replTest.awaitReplication();

    const session = primary.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    jsTestLog("Preparing a transaction on primary that should set the multikey flag.");
    session.startTransaction();
    // This write should update the multikey flag in the catalog but we don't want it to generate
    // prepare conflicts. In general, it is always safe to set an index as multikey earlier than is
    // necessary.
    assert.commandWorked(sessionColl.insert({x: [1, 2]}));
    PrepareHelpers.prepareTransaction(session);

    jsTestLog("Switching primaries by stepping up node " + secondary);
    replTest.stepUp(secondary);
    const newPrimary = replTest.getPrimary();
    const newPrimaryColl = newPrimary.getDB(dbName)[collName];

    jsTestLog("Doing an insert on the new primary that should also try to set the multikey flag.");
    assert.commandWorked(newPrimaryColl.insert({x: [3, 4]}));
    replTest.awaitReplication();

    jsTestLog("Aborting the prepared transaction on session " + tojson(session.getSessionId()));
    assert.commandWorked(newPrimary.adminCommand({
        abortTransaction: 1,
        lsid: session.getSessionId(),
        txnNumber: session.getTxnNumber_forTesting(),
        autocommit: false
    }));

    replTest.stopSet();
}());
