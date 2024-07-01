/**
 * Helpers for aborting transactions in concurrency workloads.
 */

/**
 * Abort the transaction on the session and return result.
 */
export function abortTransaction(sessionAwareDB, txnNumber) {
    assert(sessionAwareDB.getSession() != null);

    // Don't use the given session as it might be in a state we don't want to be and
    // because we are trying to abort with arbitrary txnNumber.
    let rawDB = sessionAwareDB.getSession().getClient().getDB(sessionAwareDB.getName());

    const abortErrorCodes = [
        ErrorCodes.NoSuchTransaction,
        ErrorCodes.TransactionCommitted,
        ErrorCodes.TransactionTooOld,
        ErrorCodes.Interrupted,
        ErrorCodes.LockTimeout,
        // TransactionRouter will error when trying to abort txns that have not been started
        8027900,
        // Ignore errors that can occur when shards are removed in the background
        ErrorCodes.HostUnreachable,
        ErrorCodes.ShardNotFound
    ];
    const abortCmd = {
        abortTransaction: 1,
        lsid: sessionAwareDB.getSession().getSessionId(),
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    };
    const res = rawDB.adminCommand(abortCmd);
    return assert.commandWorkedOrFailedWithCode(
        res, abortErrorCodes, () => `cmd: ${tojson(abortCmd)}`);
}

/**
 * This function operates on the last iteration of each thread to abort any active transactions.
 */
export function cleanupOnLastIteration(data, func) {
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
                    let res = abortTransaction(data.sessionDb, i);
                    if (res.ok === 1) {
                        break;
                    }
                } catch (exceptionDuringAbort) {
                    if (activeException !== null) {
                        print('Exception occurred: in finally block while another exception ' +
                              'is active: ' + tojson(activeException));
                        print('Original exception stack trace: ' + activeException.stack);
                    }

                    /* eslint-disable-next-line */
                    throw exceptionDuringAbort;
                }
            }
        }
    }
}
