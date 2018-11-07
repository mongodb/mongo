/**
 * Helpers for aborting transactions in concurrency workloads.
 */

/**
 * Abort the transaction on the session and return result.
 */
function abortTransaction(sessionAwareDB, txnNumber, errorCodes) {
    assert(sessionAwareDB.getSession() != null);

    // Don't use the given session as it might be in a state we don't want to be and
    // because we are trying to abort with arbitrary txnNumber.
    let rawDB = sessionAwareDB.getSession().getClient().getDB(sessionAwareDB.getName());
    const res = rawDB.adminCommand({
        abortTransaction: 1,
        lsid: sessionAwareDB.getSession().getSessionId(),
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    });

    return assert.commandWorkedOrFailedWithCode(res, errorCodes, () => `cmd: ${tojson(abortCmd)}`);
}

/**
 * This function operates on the last iteration of each thread to abort any active transactions.
 */
var {cleanupOnLastIteration} = (function() {
    function cleanupOnLastIteration(data, func, abortErrorCodes) {
        let lastIteration = ++data.iteration >= data.iterations;
        let activeException = null;

        try {
            func();
        } catch (e) {
            lastIteration = true;
            activeException = e;

            throw e;
        } finally {
            if (lastIteration) {
                // Abort the latest transactions for this session as some may have been skipped due
                // to incrementing data.txnNumber. Go in increasing order, so as to avoid bumping
                // the txnNumber on the server past that of an in-progress transaction. See
                // SERVER-36847.
                for (let i = 0; i <= data.txnNumber; i++) {
                    try {
                        let res = abortTransaction(data.sessionDb, i, abortErrorCodes);
                        if (res.ok === 1) {
                            break;
                        }
                    } catch (exceptionDuringAbort) {
                        if (activeException !== null) {
                            print('Exception occurred: in finally block while another exception ' +
                                  'is active: ' + tojson(activeException));
                            print('Original exception stack trace: ' + activeException.stack);
                        }

                        throw exceptionDuringAbort;
                    }
                }
            }
        }
    }

    return {cleanupOnLastIteration};
})();