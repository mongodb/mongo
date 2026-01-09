/**
 * Overrides runCommand to retry operations that encounter FailedToSatisfyReadPreference errors,
 * which can occur when the config server primary is stepped down.
 *
 * This override is intended to be used with the sharding_csrs_continuous_config_stepdown suite.
 *
 * NOTE: Tests that deliberately stop shards and expect FailedToSatisfyReadPreference errors from
 * them should be excluded from the suite in the yml file, not handled here. See the exclusion
 * list in sharding_csrs_continuous_config_stepdown.yml for examples.
 */

import {getCommandName} from "jstests/libs/cmd_object_utils.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const kTimeout = 10 * 60 * 1000; // 10 minutes
const kInterval = 200;

// Make it easier to understand whether or not returns from the assert.soon are being retried.
const kNoRetry = true;
const kRetry = false;

// Commands that should not be retried on FailedToSatisfyReadPreference.
const kNonRetryableCommands = new Set([
    // getMore errors cannot be retried since a client may not know if previous getMore advanced
    // the cursor.
    "getMore",
    // We do not retry checkMetadataConsistency on FailedToSatisfyReadPreference
    // from check_metadata_consistency_helpers.js
    "checkMetadataConsistency",
]);

function isRetryableException(e) {
    // Check if exception or error object has FailedToSatisfyReadPreference error code
    return e.code === ErrorCodes.FailedToSatisfyReadPreference;
}

function isRetryableError(res) {
    if (isRetryableException(res)) {
        return true;
    }

    if (res.writeErrors) {
        for (const writeError of res.writeErrors) {
            if (isRetryableException(writeError)) {
                return true;
            }
        }
    }

    if (res.writeConcernError && isRetryableException(res.writeConcernError)) {
        return true;
    }

    return false;
}

function shouldRetry(cmdObj, res) {
    const cmdName = getCommandName(cmdObj);
    if (kNonRetryableCommands.has(cmdName)) {
        return false;
    }
    // Don't retry commands in transactions - the transaction logic handles retries
    if (cmdObj.hasOwnProperty("autocommit")) {
        return false;
    }

    return isRetryableError(res);
}

function runCommandWithRetries(conn, dbName, cmdName, cmdObj, func, makeFuncArgs) {
    let res;
    let attempt = 0;
    let caughtException;

    assert.soon(
        () => {
            attempt++;
            caughtException = null;

            try {
                res = func.apply(conn, makeFuncArgs(cmdObj));
            } catch (e) {
                if (isRetryableException(e)) {
                    jsTest.log.info(
                        "Retrying on FailedToSatisfyReadPreference exception from " +
                            cmdName +
                            " during config server stepdown. Attempt: " +
                            attempt,
                        {error: e.toString()},
                    );
                    caughtException = e;
                    return kRetry;
                }
                throw e;
            }

            if (shouldRetry(cmdObj, res)) {
                jsTest.log.info(
                    "Retrying on FailedToSatisfyReadPreference from " +
                        cmdName +
                        " during config server stepdown. Attempt: " +
                        attempt,
                    {res},
                );
                return kRetry;
            }

            return kNoRetry;
        },
        () =>
            "Timed out while retrying command '" +
            tojson(cmdObj) +
            "' on FailedToSatisfyReadPreference, response: " +
            tojson(res) +
            ", last exception: " +
            (caughtException ? caughtException.toString() : "none"),
        kTimeout,
        kInterval,
    );
    return res;
}

OverrideHelpers.prependOverrideInParallelShell("jstests/libs/override_methods/implicitly_retry_on_config_stepdowns.js");

OverrideHelpers.overrideRunCommand(runCommandWithRetries);
