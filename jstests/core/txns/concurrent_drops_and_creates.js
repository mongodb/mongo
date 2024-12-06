/**
 * Test that a transaction cannot write to a collection that has been dropped or created since the
 * transaction started.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: endSession.
 *   not_allowed_with_signed_security_token,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   uses_snapshot_read_concern,
 *   uses_transactions,
 *   assumes_no_track_upon_creation,
 * ]
 */
import {
    withAbortAndRetryOnTransientTxnError
} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

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
withAbortAndRetryOnTransientTxnError(session, () => {
    // Ensure collection A and collection B exist.
    assert.commandWorked(sessionCollA.insert({}));
    assert.commandWorked(sessionCollB.insert({}));

    // Start the transaction with a write to collection A.
    const txnOptions = {readConcern: {level: "snapshot"}};
    session.startTransaction(txnOptions);

    assert.commandWorked(sessionCollA.insert({}));

    // Drop collection B outside of the transaction. Advance the cluster time of the session
    // performing the drop to ensure it happens at a later cluster time than the transaction
    // began.
    sessionOutsideTxn.advanceClusterTime(session.getClusterTime());
    assert.commandWorked(testDB2.runCommand({drop: collNameB, writeConcern: {w: "majority"}}));

    // This test cause a StaleConfig error on sharding so no command will succeed.
    if (!session.getClient().isMongos() && !TestData.testingReplicaSetEndpoint) {
        // We can perform reads on the dropped collection as it existed when we started the
        // transaction.
        assert.commandWorked(sessionDB2.runCommand({find: sessionCollB.getName()}));

        // However, trying to perform a write will cause a write conflict.
        assert.commandFailedWithCode(
            sessionDB2.runCommand(
                {findAndModify: sessionCollB.getName(), update: {a: 1}, upsert: true}),
            ErrorCodes.WriteConflict);
    } else {
        // TODO (SERVER-39704): See if we can match the replicaset behaviour.
        assert.commandFailedWithCode(
            sessionDB2.runCommand(
                {findAndModify: sessionCollB.getName(), update: {a: 1}, upsert: true}),
            ErrorCodes.StaleConfig);
    }

    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
});

//
// A transaction with snapshot read concern cannot write to a collection that existed at the logical
// timestamp at which the transaction executes but was dropped at a later timestamp.
// There's a subtle difference with the test case immediately above: In this case, by the wallclock
// time the transaction starts, the collection has already been dropped; whereas in the test case
// above, the collection is dropped at a wallclock time later than the wallclock time the
// transaction starts.
//
// Skip on causal-consistency suites because we cannot use 'atClusterTime' there.
if (!db.getMongo().isCausalConsistency()) {
    withAbortAndRetryOnTransientTxnError(session, () => {
        // Ensure collection A and collection B exist.
        assert.commandWorked(sessionCollA.insert({}));
        assert.commandWorked(sessionCollB.insert({}));
        const txnReadTimestamp = session.getOperationTime();

        // Drop collection B outside of the transaction. Advance the cluster time of the session
        // performing the drop to ensure it happens at a later cluster time than the
        // transaction's.
        assert.commandWorked(testDB2.runCommand({drop: collNameB, writeConcern: {w: "majority"}}));

        // Start the transaction with a write to collection A. Use an explicit atClusterTime
        // with a timestamp at which collectionB existed and contained one document.
        sessionOutsideTxn.advanceClusterTime(session.getClusterTime());
        session.startTransaction(
            {readConcern: {level: "snapshot", atClusterTime: txnReadTimestamp}});

        // Expect a conflict to be thrown, because the collection was dropped at a logical
        // timestamp greater than the one the transaction is reading at.
        assert.commandFailedWithCode(
            sessionDB2.runCommand({findAndModify: sessionCollB.getName(), update: {a: 1}}),
            ErrorCodes.WriteConflict);
        assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);
    });
}

//
// A transaction with snapshot read concern cannot write to a collection that has been created
// since the transaction started.
//

withAbortAndRetryOnTransientTxnError(session, () => {
    // Ensure collection A exists and collection B does not exist.
    assert.commandWorked(sessionCollA.insert({}));
    testDB2.runCommand({drop: collNameB, writeConcern: {w: "majority"}});

    // Start the transaction with a write to collection A.
    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandWorked(sessionCollA.insert({}));

    // Create collection B outside of the transaction. Advance the cluster time of the session
    // performing the drop to ensure it happens at a later cluster time than the transaction
    // began.
    sessionOutsideTxn.advanceClusterTime(session.getClusterTime());
    assert.commandWorked(testDB2.runCommand({create: collNameB}));

    // We can insert to collection B in the transaction as the transaction does not have a
    // collection on this namespace (even as it exist at latest). A collection will be
    // implicitly created and we will fail to commit this transaction with a WriteConflict
    // error.
    const expectedErrorCodes = [ErrorCodes.WriteConflict];
    if (!FeatureFlagUtil.isPresentAndEnabled(db, "CreateCollectionInPreparedTransactions")) {
        // If collection A and collection B live on different shards, this transaction would
        // require two phase commit. And if this feature flag is not enabled, the transaction
        // would fail with a OperationNotSupportedInTransaction error instead of a WriteConflict
        // error.
        expectedErrorCodes.push(ErrorCodes.OperationNotSupportedInTransaction);
    }

    assert.commandWorked(sessionCollB.insert({}));
    assert.commandFailedWithCode(session.commitTransaction_forTesting(), expectedErrorCodes);
});

session.endSession();
