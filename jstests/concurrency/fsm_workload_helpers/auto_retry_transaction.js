'use strict';

var {withTxnAndAutoRetry, isKilledSessionCode} = (function() {

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
        return code === ErrorCodes.Interrupted || code === ErrorCodes.CursorKilled ||
            code === ErrorCodes.CursorNotFound;
    }

    // Returns true if the transaction can be retried with a higher transaction number after the
    // given error.
    function shouldRetryEntireTxnOnError(e, hasCommitTxnError, retryOnKilledSession) {
        if ((e.hasOwnProperty('errorLabels') &&
             e.errorLabels.includes('TransientTransactionError'))) {
            return true;
        }

        // Don't retry the entire transaction on commit errors that aren't labeled as transient
        // transaction errors because it's unknown if the commit succeeded. commitTransaction is
        // individually retryable and should be retried at a lower level (e.g.
        // network_error_and_txn_override.js or commitTransactionWithKilledSessionRetries()), so any
        // error that reached here must not be transient.
        if (hasCommitTxnError) {
            print("-=-=-=- Cannot retry entire transaction on commit transaction error without" +
                  " transient transaction error label, error: " + tojsononeline(e));
            return false;
        }

        // A network error before commit is considered a transient txn error. Network errors during
        // commit should be handled at the same level as retries of retryable writes.
        if (isNetworkError(e)) {
            return true;
        }

        if (retryOnKilledSession &&
            (isKilledSessionCode(e.code) ||
             (Array.isArray(e.writeErrors) &&
              e.writeErrors.every(writeError => isKilledSessionCode(writeError.code))))) {
            return true;
        }

        return false;
    }

    // Commits the transaction active on the given session, retrying on killed session errors if
    // configured to do so. Throws if the commit fails and cannot be retried.
    function commitTransactionWithKilledSessionRetries(session, retryOnKilledSession) {
        while (true) {
            const commitRes = session.commitTransaction_forTesting();

            // If commit fails with a killed session code, the commit must be retried because it is
            // unknown if the interrupted commit succeeded. This is safe because commitTransaction
            // is a retryable write.
            if (!commitRes.ok && retryOnKilledSession && isKilledSessionCode(commitRes.code)) {
                print("-=-=-=- Retrying commit after killed session code, sessionId: " +
                      tojsononeline(session.getSessionId()) + ", txnNumber: " +
                      tojsononeline(session.getTxnNumber_forTesting()) + ", res: " +
                      tojsononeline(commitRes));
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
    function withTxnAndAutoRetry(session, func, {
        txnOptions: txnOptions = {
            readConcern: {level: TestData.defaultTransactionReadConcernLevel || 'snapshot'},
            writeConcern: TestData.hasOwnProperty("defaultTransactionWriteConcernW")
                ? {w: TestData.defaultTransactionWriteConcernW, wtimeout: kDefaultWtimeout}
                : undefined
        },
        retryOnKilledSession: retryOnKilledSession = false,
        prepareProbability: prepareProbability = 0.0
    } = {}) {
        // Committing a manually prepared transaction isn't currently supported when sessions might
        // be killed.
        assert(!retryOnKilledSession || prepareProbability === 0.0,
               "retrying on killed session error codes isn't supported with prepareProbability");

        let hasTransientError;

        do {
            session.startTransaction_forTesting(txnOptions, {ignoreActiveTxn: true});
            let hasCommitTxnError = false;
            hasTransientError = false;

            try {
                func();

                try {
                    const rand = Random.rand();
                    if (rand < prepareProbability) {
                        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
                        PrepareHelpers.commitTransaction(session, prepareTimestamp);
                    } else {
                        commitTransactionWithKilledSessionRetries(session, retryOnKilledSession);
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
                        [ErrorCodes.NoSuchTransaction, ErrorCodes.Interrupted]);
                }

                if (shouldRetryEntireTxnOnError(e, hasCommitTxnError, retryOnKilledSession)) {
                    hasTransientError = true;
                    continue;
                }

                throw e;
            }
        } while (hasTransientError);
    }

    return {withTxnAndAutoRetry, isKilledSessionCode};
})();
