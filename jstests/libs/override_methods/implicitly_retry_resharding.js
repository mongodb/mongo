/**
 * Overrides runCommand to retry "reshardCollection", "rewriteCollection", "moveCollection", and "unshardCollection"
 * commands if they fail with retryable error codes.
 *
 * Errors like OplogQueryMinTsMissing, SnapshotUnavailable, and SnapshotTooOld can occur when
 * initial sync runs concurrently with resharding, since initial sync does not maintain pinned history.
 */

import {getCommandName} from "jstests/libs/cmd_object_utils.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const kTimeout = 20 * 60 * 1000;
const kInterval = 100;

const kRetryableCommands = ["reshardCollection", "rewriteCollection", "moveCollection", "unshardCollection"];

const kRetryableErrorCodes = [
    ErrorCodes.OplogQueryMinTsMissing,
    ErrorCodes.SnapshotUnavailable,
    ErrorCodes.SnapshotTooOld,
];

function isRetryableOplogOrSnapshotError(cmdObj, res) {
    const commandName = getCommandName(cmdObj);
    if (!kRetryableCommands.includes(commandName)) {
        return false;
    }
    if (res && kRetryableErrorCodes.includes(res.code)) {
        return true;
    }
    return false;
}

function runCommandWithRetries(conn, dbName, cmdName, cmdObj, func, makeFuncArgs) {
    let res;
    let attempt = 0;

    assert.soon(
        () => {
            attempt++;

            res = func.apply(conn, makeFuncArgs(cmdObj));

            if (isRetryableOplogOrSnapshotError(cmdObj, res)) {
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

OverrideHelpers.prependOverrideInParallelShell("jstests/libs/override_methods/implicitly_retry_resharding.js");

OverrideHelpers.overrideRunCommand(runCommandWithRetries);
