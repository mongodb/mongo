/**
 * This override checks the result of runCommand to see if an OperationNotSupportedInTransaction,
 * InvalidOptions or TransientTransactionError was returned. Some FSM workloads don't check if
 * runCommand worked because it is expected to fail when certain other operations are running. We
 * want to make sure that those errors are still ignored but not OperationNotSupportedInTransaction,
 * InvalidOptions or TransientTransactionError.
 */
(function() {
    "use strict";

    load("jstests/libs/override_methods/override_helpers.js");

    function runCommandCheckForOperationNotSupportedInTransaction(
        conn, dbName, commandName, commandObj, func, makeFuncArgs) {
        let res = func.apply(conn, makeFuncArgs(commandObj));
        if (!res.ok && ((res.code === ErrorCodes.OperationNotSupportedInTransaction) ||
                        (res.code === ErrorCodes.InvalidOptions) ||
                        (res.hasOwnProperty('errorLabels') &&
                         res.errorLabels.includes('TransientTransactionError') &&
                         res.code !== ErrorCodes.NoSuchTransaction))) {
            assert.commandWorked(res);
        }

        return res;
    }

    OverrideHelpers.prependOverrideInParallelShell(
        "jstests/libs/override_methods/check_for_operation_not_supported_in_transaction.js");

    OverrideHelpers.overrideRunCommand(runCommandCheckForOperationNotSupportedInTransaction);
})();
