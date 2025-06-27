/**
 * Overrides runCommand so operations that encounter the various error codes automatically retry,
 * when run in FCV upgrade/downgrade.
 */
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const kRetryTimeout = 10 * 60 * 1000;
const kIntervalBetweenRetries = 50;

// These are the commands that will retry on ConflictingOperationInProgress error codes.
const kRetryableCommands = new Set([
    // For $querySettings retry on 'queryShapeRepresentativeQueries' collection drop.
    "aggregate",
    "setQuerySettings",
    "removeQuerySettings",
]);

const kQueryRetryableErrors = [
    ErrorCodes.ConflictingOperationInProgress,
    ErrorCodes.QueryPlanKilled,
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

            // This handles the retry case when run against a standalone, replica set, or mongos
            // where both shards returned the same response.
            if (kQueryRetryableErrors.includes(commandResponse.code)) {
                jsTest.log.info(`Retrying the '${commandName}' command because a conflicting operation is in progress: (attempt ${attempt})`,
                                {commandObj, commandResponse});
                return kRetry;
            }

            jsTest.log.info(`Command '${commandName}' failed with error code: ${
                                commandResponse.code} and will not be retried.`,
                            {commandObj, commandResponse});
            return kNoRetry;
        },
        () => `Timed out while retrying command '${toJsonForLog(commandObj)}', response: ${
            toJsonForLog(commandResponse)}`,
        kRetryTimeout,
        kIntervalBetweenRetries);

    return commandResponse;
}

function runCommandWithRetry(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    if (typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    if (kRetryableCommands.has(commandName)) {
        return _runCommandWithRetry(conn, commandName, commandObj, func, makeFuncArgs);
    }
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.overrideRunCommand(runCommandWithRetry);
