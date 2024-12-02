import "jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js";

import {
    withTxnAndAutoRetry
} from "jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js";

Random.setRandomSeed();

export var {
    withTxnAndAutoRetryOnMongos,
    withRetryOnTransientTxnError,
    withAbortAndRetryOnTransientTxnError,
    retryOnceOnTransientOnMongos
} = (() => {
    /**
     * Runs 'func' inside of a transaction started with 'txnOptions', and automatically retries
     * until it either succeeds or the server returns a non-TransientTransactionError error
     * response.
     *
     * This function manages the full lifetime of the transaction, including starting and
     * committing the txn. Useful when the test is executing a transaction as a means to test some
     * other feature/behavior, and does not care about managing or mutating an aspect of the
     * transaction itself.
     *
     * The caller should take care to ensure 'func' doesn't modify any captured variables in a
     * speculative fashion where calling it multiple times would lead to unintended behavior.
     * The transaction started by the withTxnAndAutoRetryOnMongos() function is only known to
     * have committed after the withTxnAndAutoRetryOnMongos() function returns.
     *
     * This behaviour only applies if the client is a mongos
     *
     * TODO SERVER-39704: Once completed, the usages of this function should be revisited to
     * determine whether it is still necessary or the retries performed by MongoS make it
     * unnecessary
     */
    function withTxnAndAutoRetryOnMongos(session, func, txnOptions) {
        if (session.getClient().isMongos() || TestData.testingReplicaSetEndpoint) {
            withTxnAndAutoRetry(session, func, {txnOptions});
        } else {
            session.startTransaction(txnOptions);
            func();
            assert.commandWorked(session.commitTransaction_forTesting());
        }
    }

    /**
     * Runs 'func' and automatically runs the cleanup function and retries
     * until it either succeeds or the server returns a non-TransientTransactionError error
     * response.
     *
     * This function will not start and commit/abort the transaction, the caller is expected to
     * do so inside func. It also takes in a cleanup function that the caller can choose to specify.
     * Useful when the test case is testing transactions internals, and needs to manage or mutate
     * some aspect of the transaction itself, and needs to do some clean up in addition to just
     * aborting the transaction.
     */
    function withRetryOnTransientTxnError(func, cleanup = null) {
        assert.soon(() => {
            try {
                func();
            } catch (e) {
                if ((e.hasOwnProperty('errorLabels') &&
                     e.errorLabels.includes('TransientTransactionError'))) {
                    if (cleanup)
                        cleanup();
                    return false;
                } else {
                    throw e;
                }
            }
            return true;
        });
    }

    /**
     * Runs 'func' and automatically aborts the transaction on the passed in session and retries
     * until it either succeeds or the server returns a non-TransientTransactionError error
     * response.
     *
     * This function will not start and commit/abort the transaction, the caller is expected to
     * do so inside func. It will abort the transaction in between retries. Useful when the test
     * case is testing transactions internals, and needs to manage or mutate some aspect of the
     * transaction itself.
     */
    function withAbortAndRetryOnTransientTxnError(session, func) {
        withRetryOnTransientTxnError(func, () => {
            session.abortTransaction();
        });
    }

    /**
     * Runs 'func' and retries it only once if a transient error occurred.
     *
     * This behaviour only applies if the client is a mongos
     *
     * TODO SERVER-39704: Once completed, the usages of this function should be revisited to
     * determine whether it is still necessary or the retries performed by MongoS make it
     * unnecessary
     *
     * This function will not start and commit/abort the transaction, the caller is expected to
     * do so inside func. Useful when the test case is testing transactions internals, and needs to
     * manage or mutate some aspect of the transaction itself, and needs to enforce that a
     * transient error doesn't continually occur.
     */
    function retryOnceOnTransientOnMongos(session, func) {
        if (session.getClient().isMongos() || TestData.testingReplicaSetEndpoint) {
            try {
                func();
            } catch (e) {
                if ((e.hasOwnProperty('errorLabels') &&
                     e.errorLabels.includes('TransientTransactionError'))) {
                    func();
                } else {
                    throw e;
                }
            }
        } else {
            func();
        }
    }

    return {
        withTxnAndAutoRetryOnMongos,
        withRetryOnTransientTxnError,
        withAbortAndRetryOnTransientTxnError,
        retryOnceOnTransientOnMongos
    };
})();
