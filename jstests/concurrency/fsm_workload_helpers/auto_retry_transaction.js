'use strict';

var {withTxnAndAutoRetry} = (function() {

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

    /**
     * Runs 'func' inside of a transaction started with 'txnOptions', and automatically retries
     * until it either succeeds or the server returns a non-TransientTransactionError error
     * response.
     *
     * The caller should take care to ensure 'func' doesn't modify any captured variables in a
     * speculative fashion where calling it multiple times would lead to unintended behavior. The
     * transaction started by the withTxnAndAutoRetry() function is only known to have committed
     * after the withTxnAndAutoRetry() function returns.
     */
    function withTxnAndAutoRetry(
        session, func, {txnOptions: txnOptions = {readConcern: {level: 'snapshot'}}} = {}) {
        let hasTransientError;

        do {
            session.startTransaction(txnOptions);
            hasTransientError = false;

            try {
                func();

                // commitTransaction() calls assert.commandWorked(), which may fail with a
                // WriteConflict error response. We therefore suppress its doassert() output.
                quietly(() => session.commitTransaction());
            } catch (e) {
                // Use the version of abortTransaction() that ignores errors. We ignore the error
                // from abortTransaction because the transaction may have implicitly been aborted by
                // the server already and will therefore return a NoSuchTransaction error response.
                // We need to call abortTransaction() in order to update the mongo shell's state
                // such that it agrees no transaction is currently in progress on this session.
                session.abortTransaction();

                if (!e.hasOwnProperty('errorLabels') ||
                    !e.errorLabels.includes('TransientTransactionError')) {
                    throw e;
                }

                hasTransientError = true;
            }
        } while (hasTransientError);
    }

    return {withTxnAndAutoRetry};
})();
