'use strict';

load('jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js');
Random.setRandomSeed();

var {
    withTxnAndAutoRetryOnMongos,
    retryOnceOnTransientOnMongos,
    retryOnceOnTransientAndRestartTxnOnMongos
} = (() => {
    /**
     * Runs 'func' inside of a transaction started with 'txnOptions', and automatically retries
     * until it either succeeds or the server returns a non-TransientTransactionError error
     * response.
     *
     * The caller should take care to ensure 'func' doesn't modify any captured variables in a
     * speculative fashion where calling it multiple times would lead to unintended behavior. The
     * transaction started by the withTxnAndAutoRetryOnMongos() function is only known to have
     * committed after the withTxnAndAutoRetryOnMongos() function returns.
     *
     * This behaviour only applies if the client is a mongos
     *
     * TODO SERVER-39704: Once completed, the usages of this function should be revisited to
     * determine whether it is still necessary or the retries performed by MongoS make it
     * unnecessary
     */
    function withTxnAndAutoRetryOnMongos(session, func, txnOptions) {
        if (session.getClient().isMongos()) {
            withTxnAndAutoRetry(session, func, {txnOptions});
        } else {
            session.startTransaction(txnOptions);
            func();
            assert.commandWorked(session.commitTransaction_forTesting());
        }
    }

    /**
     * Runs 'func' and retries it only once if a transient error occurred.
     *
     * This behaviour only applies if the client is a mongos
     *
     * TODO SERVER-39704: Once completed, the usages of this function should be revisited to
     * determine whether it is still necessary or the retries performed by MongoS make it
     * unnecessary
     */
    function retryOnceOnTransientOnMongos(session, func) {
        if (session.getClient().isMongos()) {
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

    /**
     * Runs 'func' and retries it only once restarting the transaction if a transient
     * error occurred.
     *
     * This behaviour only applies if the client is a mongos
     *
     * TODO SERVER-39704: Once completed, the usages of this function should be revisited to
     * determine whether it is still necessary or the retries performed by MongoS make it
     * unnecessary
     */
    function retryOnceOnTransientAndRestartTxnOnMongos(session, func, txnOptions) {
        if (session.getClient().isMongos()) {
            try {
                func();
            } catch (e) {
                if ((e.hasOwnProperty('errorLabels') &&
                     e.errorLabels.includes('TransientTransactionError'))) {
                    session.abortTransaction_forTesting();
                    session.startTransaction(txnOptions);
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
        retryOnceOnTransientOnMongos,
        retryOnceOnTransientAndRestartTxnOnMongos
    };
})();
