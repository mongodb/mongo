/**
 * Overrides runCommand to retry write operations that fail while a replica set is being demoted
 * from a config server replica set (CSRS) back to a plain replica set.
 *
 * During a rolling demotion, some members have already been restarted without --configsvr while
 * others still believe they are talking to a config server. Connecting to a demoted member can
 * then fail with InvalidOptions and:
 *   "Surprised to discover that <host> does not believe it is a config server"
 *
 * This is an accepted side effect of SPM-4300; this override retries it so it does not flake the
 * replica_sets_transition_to_csrs suite.
 *
 * Only retryable write commands are retried. Transactions are left to their own retry logic.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {RetryableWritesUtil} from "jstests/libs/retryable_writes_util.js";

const kTimeout = 1 * 60 * 1000;
const kInterval = 200;

const kNoRetry = true;
const kRetry = false;

const kNotConfigServerErrmsg = "does not believe it is a config server";

function getErrmsg(error) {
    if (!error || typeof error !== "object") {
        return "";
    }
    return error.errmsg || error.message || "";
}

function isCsrsTransitionMismatchError(error) {
    if (!error || error.code !== ErrorCodes.InvalidOptions) {
        return false;
    }
    const errmsg = getErrmsg(error);
    return errmsg.includes(kNotConfigServerErrmsg);
}

function responseHasCsrsTransitionMismatch(res) {
    if (!res || typeof res !== "object") {
        return false;
    }

    if (isCsrsTransitionMismatchError(res)) {
        return true;
    }

    if (res.writeErrors) {
        for (const writeError of res.writeErrors) {
            if (isCsrsTransitionMismatchError(writeError)) {
                return true;
            }
        }
    }

    if (res.writeConcernError && isCsrsTransitionMismatchError(res.writeConcernError)) {
        return true;
    }

    return false;
}

function shouldRetry(cmdName, cmdObj, res) {
    if (!RetryableWritesUtil.isRetryableWriteCmdName(cmdName)) {
        return false;
    }

    // Retries in a transaction must come from whatever is running the transaction.
    if (cmdObj.hasOwnProperty("autocommit")) {
        return false;
    }

    return responseHasCsrsTransitionMismatch(res);
}

function runCommandWithRetries(conn, dbName, cmdName, cmdObj, func, makeFuncArgs) {
    if (typeof cmdObj !== "object" || cmdObj === null) {
        return func.apply(conn, makeFuncArgs(cmdObj));
    }

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
                if (shouldRetry(cmdName, cmdObj, e)) {
                    jsTest.log.info(
                        "Retrying write '" +
                            cmdName +
                            "' after CSRS transition mismatch. Attempt: " +
                            attempt,
                        {error: e},
                    );
                    caughtException = e;
                    return kRetry;
                }
                throw e;
            }

            if (shouldRetry(cmdName, cmdObj, res)) {
                jsTest.log.info(
                    "Retrying write '" +
                        cmdName +
                        "' after CSRS transition mismatch. Attempt: " +
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
            "' on CSRS transition mismatch, response: " +
            tojson(res) +
            ", last exception: " +
            (caughtException ? caughtException.toString() : "none"),
        kTimeout,
        kInterval,
    );
    return res;
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/implicitly_retry_on_csrs_transition_errors.js",
);

OverrideHelpers.overrideRunCommand(runCommandWithRetries);
