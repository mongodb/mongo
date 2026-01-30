/**
 * Tests that extension stages cannot participate in multi-document transactions.
 *
 * Verifies that:
 * 1. Extension stages fail with OperationNotSupportedInTransaction when used to start transactions
 * 2. Extension stages fail with OperationNotSupportedInTransaction when used inside transactions
 * 3. OperationNotSupportedInTransaction errors abort the transaction (expected MongoDB behavior)
 * 4. All transaction changes are rolled back when the transaction is aborted
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   requires_replication,
 *   requires_sharding,
 *   uses_transactions,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {retryOnceOnTransientOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";

const collName = jsTestName();

describe("Extension stage transaction restrictions", function () {
    const extensionPipeline = [{$testFoo: {}}];
    let testDB;
    let testColl;
    let session;
    let sessionDb;

    before(function () {
        testDB = db.getSiblingDB(jsTestName());
        testColl = testDB[collName];

        // Set up the test collection.
        testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
        assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));
        assert.commandWorked(
            testColl.insertMany(
                [
                    {_id: 0, x: 1},
                    {_id: 1, x: 2},
                    {_id: 2, x: 3},
                ],
                {writeConcern: {w: "majority"}},
            ),
        );

        // Set up session for transaction tests.
        const sessionOptions = {causalConsistency: false};
        session = db.getMongo().startSession(sessionOptions);
        sessionDb = session.getDatabase(jsTestName());
    });

    after(function () {
        if (session) {
            session.endSession();
        }
        testDB.runCommand({drop: collName});
    });

    it("should fail when starting a transaction with extension stage", function () {
        const txnNumber = NumberLong(1);
        const startTxnCmd = {
            aggregate: collName,
            pipeline: extensionPipeline,
            cursor: {},
            readConcern: {level: "snapshot"},
            txnNumber: txnNumber,
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
        };

        assert.commandFailedWithCode(
            sessionDb.runCommand(startTxnCmd),
            ErrorCodes.OperationNotSupportedInTransaction,
            "Extension stage should not be allowed to start a transaction",
        );
    });

    it("should fail and abort transaction when using extension stage inside an active transaction", function () {
        const txnNumber = NumberLong(2);
        let stmtId = 0;

        // Start a valid transaction with a regular insert.
        retryOnceOnTransientOnMongos(session, () => {
            assert.commandWorked(
                sessionDb.runCommand({
                    insert: collName,
                    documents: [{_id: 100, x: 100}],
                    readConcern: {level: "snapshot"},
                    txnNumber: txnNumber,
                    stmtId: NumberInt(stmtId++),
                    startTransaction: true,
                    autocommit: false,
                }),
            );
        });

        // Try to use extension stage inside the active transaction.
        // This should fail and abort the transaction.
        const insideTxnCmd = {
            aggregate: collName,
            pipeline: extensionPipeline,
            cursor: {},
            txnNumber: txnNumber,
            stmtId: NumberInt(stmtId++),
            autocommit: false,
        };

        assert.commandFailedWithCode(
            sessionDb.runCommand(insideTxnCmd),
            ErrorCodes.OperationNotSupportedInTransaction,
            "Extension stage should not be allowed inside an active transaction",
        );

        // The transaction should have been aborted by the OperationNotSupportedInTransaction
        // error. Attempting to commit should fail with NoSuchTransaction.
        assert.commandFailedWithCode(
            sessionDb.adminCommand({
                commitTransaction: 1,
                txnNumber: txnNumber,
                stmtId: NumberInt(stmtId++),
                autocommit: false,
                writeConcern: {w: "majority"},
            }),
            ErrorCodes.NoSuchTransaction,
            "Transaction should have been aborted",
        );

        // Verify the insert from the transaction was NOT committed (rolled back).
        assert.eq(testColl.find({_id: 100}).itcount(), 0, "Transaction insert should have been rolled back");
    });

    it("should abort transaction when extension stage fails with OperationNotSupportedInTransaction", function () {
        const txnNumber = NumberLong(3);
        let stmtId = 0;

        // Start a transaction with an insert.
        retryOnceOnTransientOnMongos(session, () => {
            assert.commandWorked(
                sessionDb.runCommand({
                    insert: collName,
                    documents: [{_id: 200, x: 200}],
                    readConcern: {level: "snapshot"},
                    txnNumber: txnNumber,
                    stmtId: NumberInt(stmtId++),
                    startTransaction: true,
                    autocommit: false,
                }),
            );
        });

        // Attempt to use extension stage (should fail and abort the transaction).
        // OperationNotSupportedInTransaction is a transaction-aborting error.
        assert.commandFailedWithCode(
            sessionDb.runCommand({
                aggregate: collName,
                pipeline: extensionPipeline,
                cursor: {},
                txnNumber: txnNumber,
                stmtId: NumberInt(stmtId++),
                autocommit: false,
            }),
            ErrorCodes.OperationNotSupportedInTransaction,
        );

        // The transaction was aborted, so any subsequent operation should fail.
        assert.commandFailedWithCode(
            sessionDb.runCommand({
                insert: collName,
                documents: [{_id: 201, x: 201}],
                txnNumber: txnNumber,
                stmtId: NumberInt(stmtId++),
                autocommit: false,
            }),
            ErrorCodes.NoSuchTransaction,
            "Transaction should be aborted after OperationNotSupportedInTransaction error",
        );

        // Verify the insert from the aborted transaction was NOT committed.
        assert.eq(
            testColl.find({_id: 200}).itcount(),
            0,
            "Insert should have been rolled back when transaction was aborted",
        );
        assert.eq(
            testColl.find({_id: 201}).itcount(),
            0,
            "Second insert should not exist (transaction was already aborted)",
        );
    });
});
