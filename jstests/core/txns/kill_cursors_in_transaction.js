// Tests that the killCursors command is allowed in transactions.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: endSession, killCursors.
//   not_allowed_with_signed_security_token,
//   uses_transactions,
//   uses_parallel_shell
// ]

import {
    withRetryOnTransientTxnError,
    withTxnAndAutoRetryOnMongos,
} from "jstests/libs/auto_retry_transaction_in_sharding.js";

const dbName = "test";
const collName = "kill_cursors_in_transaction";
const testDB = db.getSiblingDB(dbName);
const adminDB = db.getSiblingDB("admin");
const session = db.getMongo().startSession({causalConsistency: false});
const session2 = db.getMongo().startSession({causalConsistency: false});
const sessionDb = session.getDatabase(dbName);
const sessionDb2 = session2.getDatabase(dbName);
const sessionColl = sessionDb[collName];

sessionColl.drop({writeConcern: {w: "majority"}});
for (let i = 0; i < 4; ++i) {
    assert.commandWorked(sessionColl.insert({_id: i}));
}

jsTest.log("Test that the killCursors command is allowed in transactions.");

withTxnAndAutoRetryOnMongos(
    session,
    () => {
        let res = assert.commandWorked(sessionDb.runCommand({find: collName, batchSize: 2}));
        assert(res.hasOwnProperty("cursor"), tojson(res));
        assert(res.cursor.hasOwnProperty("id"), tojson(res));
        assert.commandWorked(sessionDb.runCommand({killCursors: collName, cursors: [res.cursor.id]}));
    },
    /* txnOpts = */ {},
);

jsTest.log("Test that the killCursors cannot be the first operation in a transaction.");
let res = assert.commandWorked(sessionDb.runCommand({find: collName, batchSize: 2}));
assert(res.hasOwnProperty("cursor"), tojson(res));
assert(res.cursor.hasOwnProperty("id"), tojson(res));
withRetryOnTransientTxnError(
    () => {
        session.startTransaction();
        assert.commandFailedWithCode(
            sessionDb.runCommand({killCursors: collName, cursors: [res.cursor.id]}),
            ErrorCodes.OperationNotSupportedInTransaction,
        );
        assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
    },
    () => {
        session.abortTransaction_forTesting();
    },
);

// TODO SERVER-102866 remove when 9.0 becomes last-lts
const isMultiversion =
    Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) || Boolean(TestData.multiversionBinVersion);
if (!isMultiversion) {
    jsTest.log("killcursors in session but not transaction can kill cursor in a transaction in another session");
    withRetryOnTransientTxnError(
        () => {
            session.startTransaction();

            // Open a cursor on the collection.
            let res = assert.commandWorked(sessionDb.runCommand({find: collName, batchSize: 2}));
            assert(res.hasOwnProperty("cursor"), tojson(res));
            assert(res.cursor.hasOwnProperty("id"), tojson(res));

            assert.commandWorked(sessionDb2.runCommand({killCursors: collName, cursors: [res.cursor.id]}));
        },
        () => {
            session.abortTransaction_forTesting();
        },
    );
    session.abortTransaction_forTesting();

    jsTest.log(
        "killcursors in session1 in transaction should not be able to kill cursor in session2 since " +
            "it's not from the same transaction",
    );
    withRetryOnTransientTxnError(
        () => {
            session.startTransaction();

            // Open cursor in just so the killcursors isn't the first op in the transaction
            assert.commandWorked(sessionDb.runCommand({find: collName, batchSize: 2}));

            // Open a cursor in the other session.
            let res = assert.commandWorked(sessionDb2.runCommand({find: collName, batchSize: 2}));
            assert(res.hasOwnProperty("cursor"), tojson(res));
            assert(res.cursor.hasOwnProperty("id"), tojson(res));

            assert.commandFailedWithCode(
                sessionDb.runCommand({killCursors: collName, cursors: [res.cursor.id]}),
                8912345,
            );
        },
        () => {
            session.abortTransaction_forTesting();
        },
    );
    session.abortTransaction_forTesting();

    jsTest.log(
        "killcursors in transaction should not be able to kill cursor" + "in another transaction in the same session",
    );
    withRetryOnTransientTxnError(
        () => {
            session.startTransaction();

            // Open cursor in txn A
            res = assert.commandWorked(sessionDb.runCommand({find: collName, batchSize: 2}));
        },
        () => {
            session.abortTransaction_forTesting();
        },
    );

    withRetryOnTransientTxnError(
        () => {
            session.startTransaction_forTesting({}, {ignoreActiveTxn: true});

            // Open cursor in just so the killcursors isn't the first op in the transaction
            assert.commandWorked(sessionDb.runCommand({find: collName, batchSize: 2}));

            assert.commandFailedWithCode(
                sessionDb.runCommand({killCursors: collName, cursors: [res.cursor.id]}),
                8912321,
            );
        },
        () => {
            session.abortTransaction_forTesting();
        },
    );
    session.abortTransaction_forTesting();
}

jsTest.log("killCursors must not block on locks held by the transaction in which it is run.");

withRetryOnTransientTxnError(
    () => {
        session.startTransaction();

        // Open a cursor on the collection.
        res = assert.commandWorked(sessionDb.runCommand({find: collName, batchSize: 2}));
        assert(res.hasOwnProperty("cursor"), tojson(res));
        assert(res.cursor.hasOwnProperty("id"), tojson(res));
    },
    () => {
        session.abortTransaction_forTesting();
    },
);

// Start a drop, which will hang.
let awaitDrop = startParallelShell(function () {
    db.getSiblingDB("test")["kill_cursors_in_transaction"].drop({writeConcern: {w: "majority"}});
});

// Wait for the drop to have a pending MODE_X lock on the database.
assert.soon(
    function () {
        return (
            adminDB
                .aggregate([
                    {$currentOp: {}},
                    {
                        $match: {
                            $or: [
                                {"command.drop": collName, waitingForLock: true},
                                {"command._shardsvrParticipantBlock": collName},
                            ],
                        },
                    },
                ])
                .itcount() > 0
        );
    },
    function () {
        return "Failed to find drop in currentOp output: " + tojson(adminDB.aggregate([{$currentOp: {}}]).toArray());
    },
);

// killCursors does not block behind the pending MODE_X lock. It is possible that due to ticket
// exhaustion we end up detecting a deadlocked state, where the drop operation is waiting for
// an X collection lock but cannot acquire it because IX locks are being held by the killCursor
// operation, in which case we fail the killCursor command. If there was an error running the
// command below we should ensure that is a TransientTransactionError with a code of LockTimeOut,
// and ensure that the transaction was successfully rolled back.
res = sessionDb.runCommand({killCursors: collName, cursors: [res.cursor.id]});
if (res.ok) {
    assert.commandWorked(session.commitTransaction_forTesting());
} else {
    const isTransientTxnError =
        res.hasOwnProperty("errorLabels") && res.errorLabels.includes("TransientTransactionError");
    const isLockTimeout = res.hasOwnProperty("code") && ErrorCodes.LockTimeout === res.code;
    assert(isTransientTxnError, res);
    assert(isLockTimeout, res);
    // The transaction should have implicitly been aborted.
    assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
}

// Once the transaction has committed, the drop can proceed.
awaitDrop();

session.endSession();
