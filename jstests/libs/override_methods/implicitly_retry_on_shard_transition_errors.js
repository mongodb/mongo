/**
 * Overrides runCommand to retry operations that encounter errors from removing a shard,
 * or from a config shard transitioning in and out of dedicated mode.
 */
import {getCommandName} from "jstests/libs/cmd_object_utils.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const kTimeout = 20 * 60 * 1000;
const kInterval = 200;

// Make it easier to understand whether or not returns from the assert.soon are being retried.
const kNoRetry = true;
const kRetry = false;

const kRetryableErrors = [
    // Not all collections can be moved with moveCollection, and must be moved with movePrimary,
    // e.g. FLE state collections, so operations against them may fail with MovePrimaryInProgress.
    {code: ErrorCodes.MovePrimaryInProgress},
    {code: ErrorCodes.ReshardCollectionInProgress},
    {
        code: ErrorCodes.ConflictingOperationInProgress,
        errmsg: "Another ConfigsvrCoordinator with different arguments is already running"
    },
    // A query can be killed if it is still selecting a query plan after a config transition has
    // completed range deletion and drops the collection. Since orphanCleanUpDelaySecs is set to be
    // lower in testing than in production, dropCollection is scheduled almost immediately.
    // Similarly, the collection rename step in resharding (moveCollection) can cause a query to
    // get killed.
    {code: ErrorCodes.QueryPlanKilled},
    // TODO SERVER-90609: Stop ignoring this error. Currently an index build may fail because a
    // concurrent movePrimary triggered by the transition hook drops the collection.
    {code: ErrorCodes.IndexBuildAborted},
    // When shards are removed, it might take some time until all routing information is updated.
    // TODO SERVER-85145: Stop ignoring transient errors that might occur with concurrent shard
    // removals.
    {code: ErrorCodes.HostUnreachable},
    {code: ErrorCodes.ShutdownInProgress},
    {code: ErrorCodes.ShardNotFound},
    {code: ErrorCodes.AddOrRemoveShardInProgress}
];

const kCommandRetryableOnShardNotFoundError = {
    // The function in the KV pair's value must return true if the command is retryable
    "moveChunk": (command) => {
        // only retryable if the target is the config shard (as eventually it will show up again)
        return command.toShard == "config";
    },
    "moveRange": (command) => {
        // only retryable if the target is the config shard (as eventually it will show up again)
        return command.toShard == "config";
    },
    "movePrimary": (command) => {
        // only retryable if the target is the config shard (as eventually it will show up again)
        return command.to == "config";
    },
    "moveCollection": (command) => {
        // only retryable if the target is the config shard (as eventually it will show up again)
        return command.toShard == "config";
    },
    "enableSharding": (command) => {
        // if the target is omitted, we can retry
        if (!command.hasOwnProperty["primaryShard"]) {
            return true;
        }
        // only retryable if the target is the config shard (as eventually it will show up again)
        return command.primaryShard == "config";
    },
    "reshardCollection": (command) => {
        // if the target is omitted, we can retry
        if (!command.hasOwnProperty["shardDistribution"]) {
            return true;
        }
        // if we have more than one target, it's non-retryable
        if (command.shardDistribution.length > 1) {
            return false;
        }
        // if we have one target, only retryable if the target is the config shard (as eventually it
        // will show up again)
        return command.shardDistribution[0].shard == "config";
    }
};

// Commands known not to work with transitions so tests can fail immediately with a clear error.
// Empty for now.
const kDisallowedCommandsInsideTxns = [];
const kDisallowedCommandsOutsideTxns = ["getMore"];

function matchesRetryableError(error, retryableError) {
    for (const key of Object.keys(retryableError)) {
        if (!error.hasOwnProperty(key) || error[key] != retryableError[key]) {
            return false;
        }
    }
    return true;
}

function isRetryableError(cmdObj, error) {
    for (const retryableError of kRetryableErrors) {
        if (matchesRetryableError(error, retryableError)) {
            // we only have special handlers for the ShardNotFound, so otherwise it's retryable
            if (retryableError.code != ErrorCodes.ShardNotFound) {
                return true;
            }

            const commandName = getCommandName(cmdObj);
            // if it has no special handler, we can retry
            if (!kCommandRetryableOnShardNotFoundError.hasOwnProperty(commandName)) {
                return true;
            }

            return kCommandRetryableOnShardNotFoundError[commandName](cmdObj);
        }
    }
    return false;
}

function shouldRetry(cmdObj, res) {
    if (cmdObj.hasOwnProperty("autocommit")) {
        // Retries in a transaction must come from whatever is running the transaction.
        return false;
    }

    // getMore errors cannot be retried since a client may not know if previous getMore advanced the
    // cursor.
    if (getCommandName(cmdObj) === "getMore") {
        return false;
    }

    if (isRetryableError(cmdObj, res)) {
        return true;
    }

    const isRetryableWrite = cmdObj.hasOwnProperty("lsid") && cmdObj.hasOwnProperty("txnNumber");

    if (res.hasOwnProperty("writeErrors")) {
        // If the write isn't retryable and the batch size is > 1, we may double apply writes in the
        // batch if we just blindly retry the entire batch, since some writes in the batch might
        // have been successful. We allow retrying batch inserts because we handle duplicate key
        // errors for inserts in runCommandWithRetries below - this doesn't necessarily guarantee
        // that the inserts aren't double applied, but allows for more tests to run in suites that
        // use this override (and has not proven a problem thus far).
        if (!isRetryableWrite && (cmdObj.update && cmdObj.updates.length > 1) ||
            (cmdObj.delete && cmdObj.deletes.length > 1)) {
            return false;
        }

        for (const writeError of res.writeErrors) {
            if (isRetryableError(cmdObj, writeError)) {
                return true;
            }
        }
    }

    if (res.hasOwnProperty("cursor") && res.cursor.hasOwnProperty("firstBatch")) {
        if (!isRetryableWrite)
            return false;

        for (let opRes of res.cursor.firstBatch) {
            if (isRetryableError(cmdObj, opRes)) {
                return true;
            }
        }
    }

    return false;
}

function runCommandWithRetries(conn, dbName, cmdName, cmdObj, func, makeFuncArgs) {
    let res;
    let attempt = 0;

    const inTransaction = cmdObj.hasOwnProperty("autocommit");
    const disallowedCommands =
        inTransaction ? kDisallowedCommandsInsideTxns : kDisallowedCommandsOutsideTxns;

    if (disallowedCommands.includes(cmdName)) {
        throw new Error(`Cowardly refusing to run command ${
            (inTransaction
                 ? "inside"
                 : "outside")} of transaction with a transitioning shard ${tojson(cmdObj)}`);
    }

    assert.soon(
        () => {
            attempt++;

            res = func.apply(conn, makeFuncArgs(cmdObj));

            if (shouldRetry(cmdObj, res)) {
                jsTest.log.info("Retrying on error from " + cmdName +
                                    " with transitioning shard. Attempt: " + attempt,
                                {res});
                return kRetry;
            }

            // Some workloads use bulk inserts to load data and can fail with duplicate key errors
            // on retry if the first attempt failed part way through because of a movePrimary.
            if (cmdName === "insert" && attempt > 1 && res.hasOwnProperty("writeErrors")) {
                const hasOnlyDuplicateKeyErrors = res.writeErrors.every((err) => {
                    return err.code === ErrorCodes.DuplicateKey;
                });
                if (hasOnlyDuplicateKeyErrors) {
                    res.n += res.writeErrors.length;
                    delete res.writeErrors;
                }
                jsTest.log("No longer retrying " + cmdName +
                           " due to non-retryable error: " + tojson(res));
            }

            return kNoRetry;
        },
        () => "Timed out while retrying command '" + tojson(cmdObj) + "', response: " + tojson(res),
        kTimeout,
        kInterval);
    return res;
}

OverrideHelpers.overrideRunCommand(runCommandWithRetries);
