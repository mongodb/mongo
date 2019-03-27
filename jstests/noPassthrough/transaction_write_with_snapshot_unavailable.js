/**
 * Test that write errors in a transaction due to SnapshotUnavailable are labelled
 * TransientTransactionError and the error is reported at the top level, not in a writeErrors array.
 *
 * Other transient transaction errors are tested elsewhere: WriteConflict is tested in
 * transactions_write_conflicts.js, NotMaster is tested in transient_txn_error_labels.js, and
 * NoSuchTransaction is tested in transient_txn_error_labels_with_write_concern.js.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";
    load("jstests/libs/check_log.js");

    const name = "transaction_write_with_snapshot_unavailable";
    const replTest = new ReplSetTest({name: name, nodes: 1});
    replTest.startSet();
    replTest.initiate();

    const dbName = name;
    const dbNameB = dbName + "B";
    const collName = "collection";
    const collNameB = collName + "B";

    const primary = replTest.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB[collName].insertOne({}, {writeConcern: {w: "majority"}}));

    function testOp(cmd) {
        let op = Object.getOwnPropertyNames(cmd)[0];
        let session = primary.startSession();
        let sessionDB = session.getDatabase(name);

        jsTestLog(
            `Testing that SnapshotUnavailable during ${op} is labelled TransientTransactionError`);

        session.startTransaction({readConcern: {level: "snapshot"}});
        assert.commandWorked(sessionDB.runCommand({insert: collName, documents: [{}]}));
        // Create collection outside transaction, cannot write to it in the transaction
        assert.commandWorked(primaryDB.getSiblingDB(dbNameB).runCommand({create: collNameB}));

        let res;
        try {
            res = sessionDB.getSiblingDB(dbNameB).runCommand(cmd);
            assert.commandFailedWithCode(res, ErrorCodes.SnapshotUnavailable);
            assert.eq(res.ok, 0);
            assert(!res.hasOwnProperty("writeErrors"));
            assert.eq(res.errorLabels, ["TransientTransactionError"]);
        } catch (ex) {
            printjson(cmd);
            printjson(res);
            throw ex;
        }

        assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);
        assert.commandWorked(primaryDB.getSiblingDB(dbNameB).runCommand(
            {dropDatabase: 1, writeConcern: {w: "majority"}}));
    }

    testOp({insert: collNameB, documents: [{_id: 0}]});
    testOp({update: collNameB, updates: [{q: {}, u: {$set: {x: 1}}}]});
    testOp({delete: collNameB, deletes: [{q: {_id: 0}, limit: 1}]});

    replTest.stopSet();
})();
