/**
 * Overrides runCommand to retry "reshardCollection", "rewriteCollection", "moveCollection", and "unshardCollection"
 * commands if they fail with retryable error codes.
 *
 * Errors like OplogQueryMinTsMissing, SnapshotUnavailable, and SnapshotTooOld can occur when
 * initial sync runs concurrently with resharding, since initial sync does not maintain pinned history.
 *
 * mongos only retries a limited number of times internally, so it can return transient errors like
 * FailedToSatisfyReadPreference or MaxTimeMSExpired after a failover while resharding is still
 * running. Rerunning the same command either joins the in-progress operation or is a no-op if it
 * has already committed.
 *
 * TODO (SERVER-112761): Remove this override once reshardCollection retries these errors internally.
 */

import {getCommandName} from "jstests/libs/cmd_object_utils.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const kTimeout = 20 * 60 * 1000;
const kInterval = 100;

const kRetryableCommands = [
    "reshardCollection",
    "rewriteCollection",
    "moveCollection",
    "unshardCollection",
];

// TODO (SERVER-107404): Stop retrying on the oplog and snapshot errors below.
const kRetryableErrorCodes = [
    ErrorCodes.OplogQueryMinTsMissing,
    ErrorCodes.SnapshotUnavailable,
    ErrorCodes.SnapshotTooOld,
];

const kRetryableErrorNames = ["OplogQueryMinTsMissing", "SnapshotUnavailable", "SnapshotTooOld"];

function isRetryableOplogOrSnapshotError(cmdObj, res) {
    const commandName = getCommandName(cmdObj);
    if (!kRetryableCommands.includes(commandName)) {
        return false;
    }
    // Rerunning a forceRedistribution without an explicit reshardingUUID generates a new UUID and
    // starts a new operation, so it must not be retried. rewriteCollection is still retried because
    // the errors below are returned before the operation commits.
    if (cmdObj.forceRedistribution === true && cmdObj.reshardingUUID === undefined) {
        return false;
    }
    if (res && kRetryableErrorCodes.includes(res.code)) {
        return true;
    }
    if (res.code === ErrorCodes.ReshardCollectionTruncatedError) {
        for (const errName of kRetryableErrorNames) {
            if (res.errmsg.includes(errName)) {
                return true;
            }
        }
    }
    return false;
}

// Mirrors the errors that sharding DDL coordinators treat as retriable internally. Interrupted is
// excluded since tests expect it when an FCV change or killOp aborts resharding. Only
// WriteConcernTimeout is retried: the other write concern errors are permanent validation
// failures that will not succeed on retry.
function isTransientError(code) {
    return (
        code !== ErrorCodes.Interrupted &&
        (ErrorCodes.isRetriableError(code) ||
            ErrorCodes.isShutdownError(code) ||
            ErrorCodes.isInterruption(code) ||
            ErrorCodes.isCancellationError(code) ||
            ErrorCodes.isExceededTimeLimitError(code) ||
            code === ErrorCodes.WriteConcernTimeout ||
            ErrorCodes.isCursorInvalidatedError(code) ||
            code === ErrorCodes.FailedToSatisfyReadPreference ||
            code === ErrorCodes.LockBusy)
    );
}

// Rerunning a command after it has committed must not start another resharding operation or fail a
// precondition that the committed operation invalidated.
function isIdempotent(cmdName, cmdObj) {
    if (cmdName === "rewriteCollection") {
        return false;
    }
    if (cmdObj.forceRedistribution && cmdObj.reshardingUUID === undefined) {
        return false;
    }
    // Committing replaces the collection UUID, so a rerun fails with CollectionUUIDMismatch.
    if (cmdObj.collectionUUID !== undefined) {
        return false;
    }
    // Without toShard, each attempt picks the least-loaded shard, which can change after commit and
    // make a rerun fail with NamespaceNotSharded.
    if (cmdName === "unshardCollection" && cmdObj.toShard === undefined) {
        return false;
    }
    return true;
}

function isRetryableTransientError(cmdName, cmdObj, res) {
    // A write concern failure is reported with ok: 1 and a nested writeConcernError field rather
    // than a top-level error code.
    const errorCode = res.ok ? res.writeConcernError?.code : res.code;
    return (
        kRetryableCommands.includes(cmdName) &&
        isTransientError(errorCode) &&
        // A command with its own time limit expects to fail if resharding does not finish in time.
        cmdObj.maxTimeMS === undefined &&
        isIdempotent(cmdName, cmdObj)
    );
}

export function runCommandWithRetries(conn, dbName, cmdName, cmdObj, func, makeFuncArgs) {
    let res;
    let attempt = 0;

    assert.soon(
        () => {
            attempt++;

            try {
                res = func.apply(conn, makeFuncArgs(cmdObj));
            } catch (e) {
                // A closed connection makes runCommand throw instead of returning an error
                // response, so classify the exception like a command failure.
                const errRes = {ok: 0, code: e.code, errmsg: e.message};
                if (isRetryableTransientError(cmdName, cmdObj, errRes)) {
                    jsTest.log(
                        `Retrying ${cmdName} due to error: ${tojson(errRes)}. Attempt: ${attempt}`,
                    );
                    return false; // Retry.
                }
                throw e;
            }

            if (
                isRetryableOplogOrSnapshotError(cmdObj, res) ||
                isRetryableTransientError(cmdName, cmdObj, res)
            ) {
                jsTest.log(`Retrying ${cmdName} due to error: ${tojson(res)}. Attempt: ${attempt}`);
                return false; // Retry.
            }

            return true; // Not retry.
        },
        () => "Timed out while retrying command '" + tojson(cmdObj) + "', response: " + tojson(res),
        kTimeout,
        kInterval,
    );
    return res;
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/implicitly_retry_resharding.js",
);
OverrideHelpers.prependOverrideInThread(
    "jstests/libs/override_methods/implicitly_retry_resharding.js",
);

OverrideHelpers.overrideRunCommand(runCommandWithRetries);
