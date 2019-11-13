/**
 * Helper functions for handling errors that can only occur if commands are run inside multi-
 * statement transactions.
 */
function assertWorkedHandleTxnErrors(res, errorCodes) {
    if (TestData.runInsideTransaction) {
        assertAlways.commandWorkedOrFailedWithCode(res, errorCodes);
    } else {
        assertAlways.commandWorked(res);
    }
}

function assertWorkedOrFailedHandleTxnErrors(res, errorCodesTxn, errorCodes) {
    if (TestData.runInsideTransaction) {
        assertAlways.commandWorkedOrFailedWithCode(res, errorCodesTxn);
    } else {
        assertAlways.commandWorkedOrFailedWithCode(res, errorCodes);
    }
}
