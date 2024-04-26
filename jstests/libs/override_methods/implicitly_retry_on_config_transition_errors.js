/**
 * Overrides runCommand so operations that encounter errors from a config shard transitioning in and
 * out of dedicated mode retry.
 */
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const kTimeout = 20 * 60 * 1000;
const kInterval = 200;

// Make it easier to understand whether or not returns from the assert.soon are being retried.
const kNoRetry = true;
const kRetry = false;

// TODO SERVER-89555: Remove when operations on tracked unsharded collections don't hit
// MovePrimaryInProgress errors.
const kRetryableErrors = [ErrorCodes.MovePrimaryInProgress];

// Commands known not to work with transitions so tests can fail immediately with a clear error.
// Empty for now.
const kDisallowedCommands = [];

function shouldRetry(cmdObj, res) {
    if (cmdObj.hasOwnProperty("autocommit")) {
        // Retries in a transaction must come from whatever is running the transaction.
        return false;
    }

    if (res.hasOwnProperty("code") && kRetryableErrors.includes(res.code)) {
        return true;
    }

    if (res.hasOwnProperty("writeErrors")) {
        for (let writeError of res.writeErrors) {
            if (kRetryableErrors.includes(writeError.code)) {
                return true;
            }
        }
    }

    return false;
}

function runCommandWithRetries(conn, dbName, cmdName, cmdObj, func, makeFuncArgs) {
    let res;
    let attempt = 0;

    if (kDisallowedCommands.includes(cmdName)) {
        throw new Error("Cowardly refusing to run command with a transitioning config shard" +
                        tojson(cmdObj));
    }

    assert.soon(
        () => {
            attempt++;

            res = func.apply(conn, makeFuncArgs(cmdObj));

            if (shouldRetry(cmdObj, res)) {
                print("Retrying on error from " + cmdName +
                      " with transitioning config shard. Attempt: " + attempt +
                      ", res: " + tojson(res));
                return kRetry;
            }

            // TODO SERVER-89555: Remove once we never retry on MovePrimaryInProgress. Several
            // workloads use bulk inserts to load data and can fail with duplicate key errors on
            // retry if the first attempt failed part way through because of a movePrimary.
            if (cmdName === "insert" && attempt > 1 && res.hasOwnProperty("writeErrors")) {
                const hasOnlyDuplicateKeyErrors = res.writeErrors.every((err) => {
                    return err.code === ErrorCodes.DuplicateKey;
                });
                if (hasOnlyDuplicateKeyErrors) {
                    res.n += res.writeErrors.length;
                    delete res.writeErrors;
                }
            }

            return kNoRetry;
        },
        () => "Timed out while retrying command '" + tojson(cmdObj) + "', response: " + tojson(res),
        kTimeout,
        kInterval);
    return res;
}

OverrideHelpers.overrideRunCommand(runCommandWithRetries);
