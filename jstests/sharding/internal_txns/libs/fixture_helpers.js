function runTxnRetryOnTransientError(txnFunc) {
    assert.soon(() => {
        try {
            txnFunc();
            return true;
        } catch (e) {
            if (e.hasOwnProperty('errorLabels') &&
                e.errorLabels.includes('TransientTransactionError')) {
                jsTest.log("Failed to run transaction due to a transient error " + tojson(e));
                return false;
            } else {
                throw e;
            }
        }
    });
}
