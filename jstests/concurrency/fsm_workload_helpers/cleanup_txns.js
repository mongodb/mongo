/**
 * Helpers for aborting transactions in concurrency workloads.
 */

/**
 * Abort the transaction on the session and return result.
 */
function abortTransaction(db, txnNumber, errorCodes) {
    const abortCmd = {abortTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false};
    const res = db.adminCommand(abortCmd);
    return assert.commandWorkedOrFailedWithCode(res, errorCodes, () => `cmd: ${tojson(cmd)}`);
}

/**
 * This function operates on the last iteration of each thread to abort any active transactions.
 */
var {cleanupOnLastIteration} = (function() {
    function cleanupOnLastIteration(data, func) {
        const abortErrorCodes = [
            ErrorCodes.NoSuchTransaction,
            ErrorCodes.TransactionCommitted,
            ErrorCodes.TransactionTooOld
        ];

        let lastIteration = ++data.iteration >= data.iterations;
        try {
            func();
        } catch (e) {
            lastIteration = true;
            throw e;
        } finally {
            if (lastIteration) {
                // Abort the latest transactions for this session as some may have been skipped due
                // to incrementing data.txnNumber. Go in increasing order, so as to avoid bumping
                // the txnNumber on the server past that of an in-progress transaction. See
                // SERVER-36847.
                for (let i = 0; i <= data.txnNumber; i++) {
                    let res = abortTransaction(data.sessionDb, i, abortErrorCodes);
                    if (res.ok === 1) {
                        break;
                    }
                }
            }
        }
    }

    return {cleanupOnLastIteration};
})();