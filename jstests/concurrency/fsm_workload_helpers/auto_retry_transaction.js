import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {includesErrorCode} from "jstests/libs/error_code_utils.js";
import {KilledSessionUtil} from "jstests/libs/killed_session_util.js";
import {TxnUtil} from "jstests/libs/txns/txn_util.js";

export var {withTxnAndAutoRetry, isKilledSessionCode, shouldRetryEntireTxnOnError} = (function () {
    /**
     * Calls 'func' with the print() function overridden to be a no-op.
     *
     * This function is useful for silencing JavaScript backtraces that would otherwise be logged
     * from doassert() being called, even when the JavaScript exception is ultimately caught and
     * handled.
     */
    function quietly(func) {
        const printOriginal = print;
        try {
            print = Function.prototype;
            func();
        } finally {
            print = printOriginal;
        }
    }

    // Returns if the code is one that could come from a session being killed.
    function isKilledSessionCode(code) {
        return KilledSessionUtil.isKilledSessionCode(code);
    }

    // Returns true if the transaction can be retried with a higher transaction number after the
    // given error.
    function shouldRetryEntireTxnOnError(e, hasCommitTxnError, retryOnKilledSession) {
        if (TxnUtil.isTransientTransactionError(e)) {
            return true;
        }

        // Don't retry the entire transaction on commit errors that aren't labeled as transient
        // transaction errors because it's unknown if the commit succeeded. commitTransaction is
        // individually retryable and should be retried at a lower level (e.g.
        // network_error_and_txn_override.js or commitTransactionWithRetries()), so any
        // error that reached here must not be transient.
        if (hasCommitTxnError) {
            print(
                "-=-=-=- Cannot retry entire transaction on commit transaction error without" +
                    " transient transaction error label, error: " +
                    tojsononeline(e),
            );
            return false;
        }

        // A network error before commit is considered a transient txn error. Network errors during
        // commit should be handled at the same level as retries of retryable writes.
        if (isNetworkError(e)) {
            return true;
        }

        // A transaction aborted due to cache pressure can be retried, though doing so may just
        // compound the problem.
        if (e.code == ErrorCodes.TemporarilyUnavailable) {
            return true;
        }

        if (retryOnKilledSession && KilledSessionUtil.hasKilledSessionError(e)) {
            print("-=-=-=- Retrying transaction after killed session error: " + tojsononeline(e));
            return true;
        }

        // DDL operations on unsharded or unsplittable collections in a transaction can fail if a
        // movePrimary is in progress, which may happen in the config shard transition suite.
        if (TestData.shardsAddedRemoved && includesErrorCode(e, ErrorCodes.MovePrimaryInProgress)) {
            print("-=-=-=- Retrying transaction after move primary error: " + tojsononeline(e));
            return true;
        }

        // TODO SERVER-85145: Stop ignoring ShardNotFound errors that might occur with concurrent
        // shard removals.
        if (TestData.shardsAddedRemoved && includesErrorCode(e, ErrorCodes.ShardNotFound)) {
            print("-=-=-=- Retrying transaction after shard not found error: " + tojsononeline(e));
            return true;
        }

        return false;
    }

    // Commits the transaction active on the given session, retrying on killed session errors if
    // configured to do so. Also retries commitTransaction on FailedToSatisfyReadPreference error.
    // Throws if the commit fails and cannot be retried.
    function commitTransactionWithRetries(session, retryOnKilledSession) {
        while (true) {
            const commitRes = session.commitTransaction_forTesting();

            // If commit fails with a killed session code, the commit must be retried because it is
            // unknown if the interrupted commit succeeded. This is safe because commitTransaction
            // is a retryable write.
            const failedWithInterruption = !commitRes.ok && KilledSessionUtil.isKilledSessionCode(commitRes.code);
            const wcFailedWithInterruption = KilledSessionUtil.hasKilledSessionWCError(commitRes);
            if (retryOnKilledSession && (failedWithInterruption || wcFailedWithInterruption)) {
                print(
                    "-=-=-=- Retrying commit after killed session code, sessionId: " +
                        tojsononeline(session.getSessionId()) +
                        ", txnNumber: " +
                        tojsononeline(session.getTxnNumber_forTesting()) +
                        ", res: " +
                        tojsononeline(commitRes),
                );
                continue;
            }

            if (commitRes.code === ErrorCodes.FailedToSatisfyReadPreference) {
                print(
                    "-=-=-=- Retrying commit due to FailedToSatisfyReadPreference, sessionId: " +
                        tojsononeline(session.getSessionId()) +
                        ", txnNumber: " +
                        tojsononeline(session.getTxnNumber_forTesting()) +
                        ", res: " +
                        tojsononeline(commitRes),
                );
                continue;
            }

            // Use assert.commandWorked() because it throws an exception in the format expected by
            // the caller of this function if the commit failed. Committing may fail with a
            // transient error that can be retried on at a higher level, so suppress unnecessary
            // logging.
            quietly(() => {
                assert.commandWorked(commitRes);
            });

            return;
        }
    }

    // Use a "signature" value that won't typically match a value assigned in normal use. This way
    // the wtimeout set by this override is distinguishable in the server logs.
    const kDefaultWtimeout = 5 * 60 * 1000 + 789;

    /**
     * Runs 'func' inside of a transaction started with 'txnOptions', and automatically retries
     * until it either succeeds or the server returns a non-TransientTransactionError error
     * response. If retryOnKilledSession is true, the transaction will be automatically retried on
     * error codes that may come from a killed session as well. There is a probability of
     * 'prepareProbability' that the transaction is prepared before committing.
     *
     * The caller should take care to ensure 'func' doesn't modify any captured variables in a
     * speculative fashion where calling it multiple times would lead to unintended behavior. The
     * transaction started by the withTxnAndAutoRetry() function is only known to have committed
     * after the withTxnAndAutoRetry() function returns.
     */
    function withTxnAndAutoRetry(
        session,
        func,
        {
            txnOptions: txnOptions = {
                readConcern: {level: TestData.defaultTransactionReadConcernLevel || "snapshot"},
                writeConcern: TestData.hasOwnProperty("defaultTransactionWriteConcernW")
                    ? {w: TestData.defaultTransactionWriteConcernW, wtimeout: kDefaultWtimeout}
                    : undefined,
            },
            retryOnKilledSession: retryOnKilledSession = false,
            prepareProbability: prepareProbability = 0.0,
        } = {},
    ) {
        // Committing a manually prepared transaction isn't currently supported when sessions might
        // be killed.
        assert(
            !retryOnKilledSession || prepareProbability === 0.0,
            "retrying on killed session error codes isn't supported with prepareProbability",
        );

        let hasTransientError;
        let iterations = 0;
        do {
            session.startTransaction_forTesting(txnOptions, {ignoreActiveTxn: true});
            let hasCommitTxnError = false;
            hasTransientError = false;

            iterations += 1;
            if (iterations % 10 === 0) {
                print("withTxnAndAutoRetry has iterated " + iterations + " times.");
            }
            try {
                func();

                try {
                    const rand = Random.rand();
                    if (rand < prepareProbability) {
                        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
                        PrepareHelpers.commitTransaction(session, prepareTimestamp);
                    } else {
                        commitTransactionWithRetries(session, retryOnKilledSession);
                    }
                } catch (e) {
                    hasCommitTxnError = true;
                    throw e;
                }
            } catch (e) {
                if (!hasCommitTxnError) {
                    // We need to call abortTransaction_forTesting() in order to update the mongo
                    // shell's state such that it agrees no transaction is currently in progress on
                    // this session.
                    // The transaction may have implicitly been aborted by the server or killed by
                    // the kill_session helper and will therefore return a
                    // NoSuchTransaction/Interrupted error code.
                    assert.commandWorkedOrFailedWithCode(
                        session.abortTransaction_forTesting(),
                        [
                            ErrorCodes.NoSuchTransaction,
                            ErrorCodes.Interrupted,
                            // Ignore sessions killed due to cache pressure. See SERVER-100367.
                            ErrorCodes.TemporarilyUnavailable,
                            // Ignore errors that can occur when shards are removed in the background
                            ErrorCodes.HostUnreachable,
                            ErrorCodes.ShardNotFound,
                            // Ignore errors that can occur when there is a resharding operation.
                            ErrorCodes.InterruptedDueToReshardingCriticalSection,
                        ],
                        `Failed to abort transaction after transaction operations failed with error: ${tojson(e)}`,
                    );
                }

                if (shouldRetryEntireTxnOnError(e, hasCommitTxnError, retryOnKilledSession)) {
                    print("Retrying transaction due to transient error: " + tojson(e));
                    hasTransientError = true;
                    continue;
                }

                throw e;
            }
        } while (hasTransientError);
    }

    return {withTxnAndAutoRetry, isKilledSessionCode, shouldRetryEntireTxnOnError};
})();
