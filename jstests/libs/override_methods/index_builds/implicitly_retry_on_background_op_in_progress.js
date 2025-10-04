/**
 * Overrides runCommand so operations that encounter the BackgroundOperationInProgressForNs/Db error
 * codes automatically retry.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

// These are all commands that can return BackgroundOperationInProgress error codes.
const commandAllowlist = new Set([
    "cloneCollectionAsCapped",
    "collMod",
    "compact",
    "convertToCapped",
    "createIndexes",
    "drop",
    "dropDatabase",
    "dropIndexes",
    "reIndex",
    "renameCollection",
]);

// Allowlisted errors commands may encounter when retried on a sharded cluster. Shards may
// return different responses, so errors associated with repeated executions of a command may be
// ignored.
// TODO SERVER-107420: Remove IndexNotFound from acceptable dropIndexes errors once 9.0 becomes LTS
const acceptableCommandErrors = {
    "drop": [ErrorCodes.NamespaceNotFound],
    "dropIndexes": [ErrorCodes.IndexNotFound],
    "renameCollection": [ErrorCodes.NamespaceNotFound],
};

const kTimeout = 10 * 60 * 1000;
const kInterval = 200;

// Make it easier to understand whether or not returns from the assert.soon are being retried.
const kNoRetry = true;
const kRetry = false;

function hasBackgroundOpInProgress(res) {
    // Only these are retryable.
    return (
        res.code === ErrorCodes.BackgroundOperationInProgressForNamespace ||
        res.code === ErrorCodes.BackgroundOperationInProgressForDatabase
    );
}

function runCommandWithRetries(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
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
            if (!commandAllowlist.has(commandName)) {
                return kNoRetry;
            }

            let message =
                "Retrying the " +
                commandName +
                " command because a background operation is in progress (attempt " +
                attempt +
                "): " +
                tojson(res);

            // This handles the retry case when run against a standalone, replica set, or mongos
            // where both shards returned the same response.
            if (hasBackgroundOpInProgress(res)) {
                jsTest.log.info(message);
                return kRetry;
            }

            // The following logic only applies to sharded clusters.
            if (!conn.isMongos() || !res.raw) {
                // We don't attempt to retry commands for which mongos doesn't expose the raw
                // responses from the shards.
                return kNoRetry;
            }

            // In certain cases, retrying a command on a sharded cluster may result in a
            // scenario where one shard has executed the command and another still has a
            // background operation in progress. Retry, ignoring allowlisted errors on a
            // command-by-command basis.
            let shardsWithBackgroundOps = [];

            // If any shard has a background operation in progress and the other shards sent
            // allowlisted errors after a first attempt, retry the entire command.
            for (let shard in res.raw) {
                let shardRes = res.raw[shard];
                if (shardRes.ok) {
                    continue;
                }

                if (hasBackgroundOpInProgress(shardRes)) {
                    shardsWithBackgroundOps.push(shard);
                    continue;
                }

                // If any of the shards return an error that is not allowlisted or even if a
                // allowlisted error is received on the first attempt, do not retry.
                let acceptableErrors = acceptableCommandErrors[commandName] || [];
                if (!acceptableErrors.includes(shardRes.code)) {
                    return kNoRetry;
                }
                // Allowlisted errors can only occur from running a command more than once, so
                // it would be unexpected to receive an error on the first attempt.
                if (attempt === 1) {
                    return kNoRetry;
                }
            }

            // At this point, all shards have resulted in allowlisted errors resulting in
            // retrying allowlisted commands. Fake a successful response.
            if (shardsWithBackgroundOps.length === 0) {
                jsTest.log.info(
                    "done retrying " +
                        commandName +
                        " command because all shards have responded with acceptable errors",
                );
                res.ok = 1;
                return kNoRetry;
            }

            jsTest.log.info(message + " on shards", {shardsWithBackgroundOps});
            return kRetry;
        },
        () => "Timed out while retrying command '" + tojson(commandObj) + "', response: " + tojson(res),
        kTimeout,
        kInterval,
    );
    return res;
}

OverrideHelpers.overrideRunCommand(runCommandWithRetries);
