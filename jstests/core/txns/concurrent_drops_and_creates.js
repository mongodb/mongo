/**
 * Test that a transaction cannot write to a collection that has been dropped or created since the
 * transaction started.
 *
 * The test runs commands that are not allowed with security token: endSession.
 * @tags: [
 *   not_allowed_with_security_token,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   uses_snapshot_read_concern,
 *   uses_transactions,
 * ]
 */
(function() {
"use strict";

// TODO (SERVER-39704): Remove the following load after SERVER-397074 is completed
// For retryOnceOnTransientAndRestartTxnOnMongos.
load('jstests/libs/auto_retry_transaction_in_sharding.js');
load("jstests/libs/feature_flag_util.js");

const dbName1 = "test1";
const dbName2 = "test2";
const collNameA = "coll_A";
const collNameB = "coll_B";

const sessionOutsideTxn = db.getMongo().startSession({causalConsistency: true});
const testDB1 = sessionOutsideTxn.getDatabase(dbName1);
const testDB2 = sessionOutsideTxn.getDatabase(dbName2);
testDB1.runCommand({drop: collNameA, writeConcern: {w: "majority"}});
testDB2.runCommand({drop: collNameB, writeConcern: {w: "majority"}});

const session = db.getMongo().startSession({causalConsistency: false});
const sessionDB1 = session.getDatabase(dbName1);
const sessionDB2 = session.getDatabase(dbName2);
const sessionCollA = sessionDB1[collNameA];
const sessionCollB = sessionDB2[collNameB];

//
// A transaction with snapshot read concern cannot write to a collection that has been dropped
// since the transaction started.
//

// Ensure collection A and collection B exist.
assert.commandWorked(sessionCollA.insert({}));
assert.commandWorked(sessionCollB.insert({}));

// Start the transaction with a write to collection A.
const txnOptions = {
    readConcern: {level: "snapshot"}
};
session.startTransaction(txnOptions);

// TODO (SERVER-39704): We use the retryOnceOnTransientAndRestartTxnOnMongos
// function to handle how MongoS will propagate a StaleShardVersion error as a
// TransientTransactionError. After SERVER-39704 is completed the
// retryOnceOnTransientAndRestartTxnOnMongos can be removed
retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
    assert.commandWorked(sessionCollA.insert({}));
}, txnOptions);

// Drop collection B outside of the transaction. Advance the cluster time of the session
// performing the drop to ensure it happens at a later cluster time than the transaction began.
sessionOutsideTxn.advanceClusterTime(session.getClusterTime());
assert.commandWorked(testDB2.runCommand({drop: collNameB, writeConcern: {w: "majority"}}));

// This test cause a StaleConfig error on sharding so no command will succeed.
if (!session.getClient().isMongos()) {
    // We can perform reads on the dropped collection as it existed when we started the transaction.
    assert.commandWorked(sessionDB2.runCommand({find: sessionCollB.getName()}));

    // However, trying to perform a write will cause a write conflict.
    assert.commandFailedWithCode(
        sessionDB2.runCommand(
            {findAndModify: sessionCollB.getName(), update: {a: 1}, upsert: true}),
        ErrorCodes.WriteConflict);

    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
} else {
    // Ensure the collection drop is visible to the transaction, since our implementation of the in-
    // memory collection catalog always has the most recent collection metadata. We can detect the
    // drop by attempting a findAndModify on the dropped collection. Since the collection drop is
    // visible, the findAndModify will not match any existing documents.
    // TODO (SERVER-39704): Remove use of retryOnceOnTransientAndRestartTxnOnMongos.
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        const res = sessionDB2.runCommand(
            {findAndModify: sessionCollB.getName(), update: {a: 1}, upsert: true});
        assert.commandWorked(res);
        assert.eq(res.value, null);
    }, txnOptions);
    assert.commandWorked(session.commitTransaction_forTesting());
}

//
// A transaction with snapshot read concern cannot write to a collection that has been created
// since the transaction started.
//

// Ensure collection A exists and collection B does not exist.
assert.commandWorked(sessionCollA.insert({}));
testDB2.runCommand({drop: collNameB, writeConcern: {w: "majority"}});

// Start the transaction with a write to collection A.
session.startTransaction({readConcern: {level: "snapshot"}});
assert.commandWorked(sessionCollA.insert({}));

// Create collection B outside of the transaction. Advance the cluster time of the session
// performing the drop to ensure it happens at a later cluster time than the transaction began.
sessionOutsideTxn.advanceClusterTime(session.getClusterTime());
assert.commandWorked(testDB2.runCommand({create: collNameB}));

// We can insert to collection B in the transaction as the transaction does not have a collection on
// this namespace (even as it exist at latest). A collection will be implicitly created and we will
// fail to commit this transaction with a WriteConflict error.
retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
    assert.commandWorked(sessionCollB.insert({}));
}, txnOptions);

assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.WriteConflict);

session.endSession();
}());
