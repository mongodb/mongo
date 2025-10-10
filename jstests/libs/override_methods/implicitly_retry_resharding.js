/**
 * Overrides runCommand to retry "reshardCollection", "moveCollection", and "unshardCollection"
 * commands if they fail with OplogQueryMinTsMissing or SnapshotUnavailable.
 */

import {getCommandName} from "jstests/libs/cmd_object_utils.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const kTimeout = 20 * 60 * 1000;
const kInterval = 100;

const kRetryableCommands = ["reshardCollection", "moveCollection", "unshardCollection"];

const kRetryableErrorCodes = [ErrorCodes.OplogQueryMinTsMissing, ErrorCodes.SnapshotUnavailable];

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
