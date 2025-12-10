/**
 * Overrides runCommand so operations that encounter the various error codes automatically retry,
 * when run in FCV upgrade/downgrade.
 */
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const kRetryTimeout = 60 * 1000;
const kIntervalBetweenRetries = 50;

const kRetryableErrors = [
    {
        code: ErrorCodes.ConflictingOperationInProgress,
        errmsg: "Another ConfigsvrCoordinator with different arguments is already running",
    },
    {
        code: ErrorCodes.ConflictingOperationInProgress,
        errmsg: "Cannot downgrade while cluster server parameters are being set",
    },
    {
        code: ErrorCodes.CannotDowngrade,
        errmsg: "Cannot downgrade while cluster server parameters are being set",
    },
];

function _runCommandWithRetry(conn, commandName, commandObj, func, makeFuncArgs) {
    const kNoRetry = true;
    const kRetry = false;

    let commandResponse;
    let attempt = 0;

    assert.soon(
        () => {
            attempt++;

            commandResponse = func.apply(conn, makeFuncArgs(commandObj));
            if (commandResponse.ok === 1) {
                return kNoRetry;
            }

            for (const retryableError of kRetryableErrors) {
                if (
                    commandResponse.code === retryableError.code &&
                    commandResponse.hasOwnProperty("errmsg") &&
                    commandResponse.errmsg == retryableError.errmsg
                ) {
                    jsTest.log.info(
                        `Retrying the '${commandName}' command because a conflicting operation is in progress: (attempt ${attempt})`,
                        {commandObj, commandResponse},
                    );
                    return kRetry;
                }
            }

            jsTest.log.info(
                `Command '${commandName}' failed with error code: ${commandResponse.code} and will not be retried.`,
                {commandObj, commandResponse},
            );
            return kNoRetry;
        },
        () =>
            `Timed out while retrying command '${toJsonForLog(commandObj)}', response: ${toJsonForLog(
                commandResponse,
            )}`,
        kRetryTimeout,
        kIntervalBetweenRetries,
    );

    return commandResponse;
}

function runCommandWithRetry(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    if (typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    return _runCommandWithRetry(conn, commandName, commandObj, func, makeFuncArgs);
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/implicitly_retry_on_conflicting_operation_during_fuzztest.js",
);

OverrideHelpers.overrideRunCommand(runCommandWithRetry);
