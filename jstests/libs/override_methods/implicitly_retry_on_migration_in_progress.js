/**
 * Overrides runCommand so operations that encounter the BackgroundOperationInProgressForNs/Db error
 * codes automatically retry.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

// These are all commands that can return BackgroundOperationInProgress error codes.
const kCommandsToRetry = new Set([
    "createIndexes",
    "moveCollection",
    "reshardCollection",
    "unshardCollection",
    "find",
    "aggregate",
]);

const kErrorsToRetry = new Set([
    ErrorCodes.ReshardCollectionInProgress,
    ErrorCodes.ConflictingOperationInProgress,
    // Queries may return a QueryPlanKilled errors if there are background moveCollections or range
    // deletions.
    ErrorCodes.QueryPlanKilled,
]);

// Commands known not to work with migrations so tests can fail immediately with a clear error.
const kDisallowedCommandsInsideTxns = [];
const kDisallowedCommandsOutsideTxns = [
    // Disallow running 'getMore' command outside a txn with random moveCollection or chunk
    // migrations in the background. This is because getMore may fail with a QueryPlanKilled error.
    // When this happens, there is no way to implicitly retry the operation since the entire
    // operation should be retried from the beginning.
    // When this is the case, the test should be tagged as `requires_getmore`.
    "getMore"
];

const kTimeout = 10 * 60 * 1000;
const kInterval = 50;  // milliseconds

// Make it easier to understand whether or not returns from the assert.soon are being retried.
const kNoRetry = true;
const kRetry = false;

// function runCommandWithRetries(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
function runCommandWithMigrationRetries(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    if (typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    const inTransaction = commandObj.hasOwnProperty("autocommit");
    const disallowedCommands =
        inTransaction ? kDisallowedCommandsInsideTxns : kDisallowedCommandsOutsideTxns;

    if (disallowedCommands.includes(commandName)) {
        throw new Error(`Cowardly refusing to run command ${
            (inTransaction ? "inside" : "outside")}
            " of transaction with random moveCollection or chunk migrations in the backgorund ${
            tojson(commandObj)}`);
    }

    let res;
    let attempt = 0;

    assert.soon(
        () => {
            attempt++;

            res = func.apply(conn, makeFuncArgs(commandObj));
            if (res.ok === 1) {
                return kNoRetry;
            }

            // Commands that are not in the allowlist should not be retried.
            if (!kCommandsToRetry.has(commandName)) {
                return kNoRetry;
            }

            // Retry if the errors is in the `kErrorsToRetry` list.
            if (kErrorsToRetry.has(res.code)) {
                jsTestLog("Retrying the " + commandName +
                              " command due to concurrent migrations/range deletions (attempt " +
                              attempt + ")",
                          {error: res});
                return kRetry;
            }

            if (attempt > 1) {
                jsTestLog("Done retrying " + commandName + " got a non-retryable error",
                          {error: res});
            }
            return kNoRetry;
        },
        () => "Timed out while retrying command '" + tojson(commandObj) +
            "', response: " + tojson(res),
        kTimeout,
        kInterval);
    return res;
}

OverrideHelpers.overrideRunCommand(runCommandWithMigrationRetries);
