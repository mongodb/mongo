function runTxnRetryOnTransientError(txnFunc) {
    assert.soon(() => {
        try {
            txnFunc();
            return true;
        } catch (e) {
            if (e.hasOwnProperty('errorLabels') &&
                e.errorLabels.includes('TransientTransactionError') &&
                e.code != ErrorCodes.NoSuchTransaction) {
                // Don't retry on a NoSuchTransaction error since it implies the transaction was
                // aborted so we should propagate the error instead.
                jsTest.log("Failed to run transaction due to a transient error " + tojson(e));
                return false;
            } else {
                throw e;
            }
        }
    });
}

function runTxnRetryOnLockTimeoutError(txnFunc) {
    assert.soon(() => {
        try {
            txnFunc();
            return true;
        } catch (e) {
            if (e.hasOwnProperty('errorLabels') &&
                e.errorLabels.includes('TransientTransactionError') &&
                e.code == ErrorCodes.LockTimeout) {
                jsTest.log("Failed to run transaction due to a transient error " + tojson(e));
                return false;
            } else {
                throw e;
            }
        }
    });
}
