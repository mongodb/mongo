/**
 * Helper functions for handling errors that can only occur if commands are run inside multi-
 * statement transactions.
 */

export function assertWorkedHandleTxnErrors(res, errorCodes) {
    if (TestData.runInsideTransaction) {
        assert.commandWorkedOrFailedWithCode(res, errorCodes);
    } else {
        assert.commandWorked(res);
    }
}

export function assertWorkedOrFailedHandleTxnErrors(res, errorCodesTxn, errorCodes) {
    if (TestData.runInsideTransaction) {
        assert.commandWorkedOrFailedWithCode(res, errorCodesTxn);
    } else {
        assert.commandWorkedOrFailedWithCode(res, errorCodes);
    }
}
