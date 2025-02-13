/**
 * Overrides Mongo.prototype.runCommand to retry on errors that come from an operation's session
 * being killed.
 */
import {KilledSessionUtil} from "jstests/libs/killed_session_util.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const mongoRunCommandOriginal = Mongo.prototype.runCommand;

Mongo.prototype.runCommand = function runCommand(dbName, cmdObj, options) {
    return runWithKilledSessionRetries(this, cmdObj, mongoRunCommandOriginal, arguments);
};

const kReadCmds = new Set(["find", "count", "aggregate", "distinct"]);

// Returns if the command should retry on killed session errors.
function shouldRetry(cmdObj) {
    if (cmdObj.hasOwnProperty("autocommit")) {
        // Transactions are retried at a higher level.
        return false;
    }

    const cmdName = Object.keys(cmdObj)[0];
    if (kReadCmds.has(cmdName) || cmdObj.hasOwnProperty("txnNumber")) {
        // Reads and retryable writes are idempotent so are safe to retry.
        return true;
    }

    return TestData.alwaysRetryOnKillSessionErrors;
}

function runWithKilledSessionRetries(mongo, cmdObj, clientFunction, clientFunctionArguments) {
    if (!shouldRetry(cmdObj)) {
        return clientFunction.apply(mongo, clientFunctionArguments);
    }

    while (true) {
        try {
            const res = clientFunction.apply(mongo, clientFunctionArguments);

            if (KilledSessionUtil.hasKilledSessionError(res)) {
                jsTest.log.info("-=-=-=- Retrying after killed session error response",
                                {cmdObj, res});
                continue;
            }

            if (KilledSessionUtil.hasKilledSessionWCError(res)) {
                jsTest.log.info(
                    "-=-=-=- Retrying after killed session write concern error response",
                    {cmdObj, res});
                continue;
            }

            return res;
        } catch (e) {
            if (KilledSessionUtil.hasKilledSessionError(e)) {
                jsTest.log.info("-=-=-=- Retrying after thrown killed session error",
                                {cmdObj, error: e});
                continue;
            }
            throw e;
        }
    }
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/retry_on_killed_session.js");
