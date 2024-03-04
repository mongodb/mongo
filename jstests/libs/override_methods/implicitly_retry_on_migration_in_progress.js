/**
 * Overrides runCommand so operations that encounter the BackgroundOperationInProgressForNs/Db error
 * codes automatically retry.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

// These are all commands that can return BackgroundOperationInProgress error codes.
const commandsToRetry = new Set([
    "createIndexes",
    "moveCollection",
    "reshardCollection",
    "unshardCollection",
]);

const kTimeout = 10 * 60 * 1000;
const kInterval = 50;  // milliseconds

// Make it easier to understand whether or not returns from the assert.soon are being retried.
const kNoRetry = true;
const kRetry = false;

function hasConflictingMigrationpInProgress(res) {
    // Only these are retryable.
    return res.code === ErrorCodes.ReshardCollectionInProgress ||
        res.code === ErrorCodes.ConflictingOperationInProgress;
}

// function runCommandWithRetries(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
function runCommandWithMigrationRetries(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    if (typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
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

            // Commands that are not in the allowlist should never fail with this error code.
            if (!commandsToRetry.has(commandName)) {
                return kNoRetry;
            }

            let message = "Retrying the " + commandName +
                " command because a migration operation is in progress (attempt " + attempt +
                "): " + tojson(res);

            // This handles the retry case when run against a standalone, replica set, or mongos
            // where both shards returned the same response.
            if (hasConflictingMigrationpInProgress(res)) {
                print(message);
                return kRetry;
            }

            print("done retrying " + commandName);
            return kNoRetry;
        },
        () => "Timed out while retrying command '" + tojson(commandObj) +
            "', response: " + tojson(res),
        kTimeout,
        kInterval);
    return res;
}

OverrideHelpers.overrideRunCommand(runCommandWithMigrationRetries);
