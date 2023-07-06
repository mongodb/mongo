/**
 * Helper functions for handling errors that can only occur if commands are run inside multi-
 * statement transactions.
 */
import {assertAlways} from "jstests/concurrency/fsm_libs/assert.js";

export function assertWorkedHandleTxnErrors(res, errorCodes) {
    if (TestData.runInsideTransaction) {
        assertAlways.commandWorkedOrFailedWithCode(res, errorCodes);
    } else {
        assertAlways.commandWorked(res);
    }
}

export function assertWorkedOrFailedHandleTxnErrors(res, errorCodesTxn, errorCodes) {
    if (TestData.runInsideTransaction) {
        assertAlways.commandWorkedOrFailedWithCode(res, errorCodesTxn);
    } else {
        assertAlways.commandWorkedOrFailedWithCode(res, errorCodes);
    }
}
