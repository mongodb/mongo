'use strict';

var {withTxnAndAutoRetryOnWriteConflict} = (function() {

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
     * until it either succeeds or the server returns a non-WriteConflict error response.
     *
     * The caller should take care to ensure 'func' doesn't modify any captured variables in a
     * speculative fashion where calling it multiple times would lead to unintended behavior. The
     * transaction started by the withTxnAndAutoRetryOnWriteConflict() function is only known to
     * have committed after the withTxnAndAutoRetryOnWriteConflict() function returns.
     */
    function withTxnAndAutoRetryOnWriteConflict(
        session, func, {txnOptions: txnOptions = {readConcern: {level: 'snapshot'}}} = {}) {
        let hasWriteConflict;

        do {
            session.startTransaction(txnOptions);
            hasWriteConflict = false;

            try {
                func();

                // commitTransaction() calls assert.commandWorked(), which may fail with a
                // WriteConflict error response. We therefore suppress its doassert() output.
                quietly(() => session.commitTransaction());
            } catch (e) {
                try {
                    // abortTransaction() calls assert.commandWorked(), which may fail with a
                    // WriteConflict error response. We therefore suppress its doassert() output.
                    quietly(() => session.abortTransaction());
                } catch (e) {
                    // We ignore the error from abortTransaction() because the transaction may have
                    // implicitly been aborted by the server already and will therefore return a
                    // NoSuchTransaction error response. We need to call abortTransaction() in order
                    // to update the mongo shell's state such that it agrees no transaction is
                    // currently in progress on this session.
                }

                if (e.code !== ErrorCodes.WriteConflict) {
                    throw e;
                }

                hasWriteConflict = true;
            }
        } while (hasWriteConflict);
    }

    return {withTxnAndAutoRetryOnWriteConflict};
})();
