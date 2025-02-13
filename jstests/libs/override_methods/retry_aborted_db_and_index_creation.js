/**
 * Overrides Mongo.prototype.runCommand to retry interrupted create index and create database
 * commands. Was modeled partly on retry_on_killed_session.js.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const mongoRunCommandOriginal = Mongo.prototype.runCommand;

Mongo.prototype.runCommand = function runCommand(dbName, cmdObj, options) {
    return runWithRetries(this, cmdObj, mongoRunCommandOriginal, arguments);
};

const kCreateIndexCmdNames = new Set(["createIndexes", "createIndex"]);
const kMaxRetryCount = 100;

// Returns if the command should retry on IndexBuildAborted errors.
function shouldRetryIndexCreateCmd(cmdObj) {
    if (cmdObj.hasOwnProperty("autocommit")) {
        // Transactions are retried at a higher level.
        return false;
    }

    const cmdName = Object.keys(cmdObj)[0];
    if (kCreateIndexCmdNames.has(cmdName)) {
        return true;
    }

    return false;
}

// Returns if the code is one that could come from an index build being aborted.
function hasIndexBuildAbortedError(res) {
    return res.code === ErrorCodes.IndexBuildAborted;
}

function shouldRetryOnInterruptedError(errOrRes) {
    return errOrRes.code === ErrorCodes.Interrupted &&
        ((errOrRes.errmsg.indexOf("Database") === 0 &&
          errOrRes.errmsg.indexOf("could not be created") > 0) ||
         errOrRes.errmsg.indexOf("Failed to read local metadata.") === 0 ||
         errOrRes.errmsg.indexOf("split failed") === 0 ||
         errOrRes.errmsg.indexOf(
             "Failed to read highest version persisted chunk for collection") === 0 ||
         errOrRes.errmsg.indexOf("Command request failed on source shard.") === 0);
}

/* Run client command with the ability to retry on a IndexBuildAborted Code
 * and InterruptedDbCreation Error.
 */
function runWithRetries(mongo, cmdObj, clientFunction, clientFunctionArguments) {
    let retryCount = 0;
    while (true) {
        const res = clientFunction.apply(mongo, clientFunctionArguments);

        if (++retryCount >= kMaxRetryCount) {
            return res;
        } else if (hasIndexBuildAbortedError(res)) {
            if (shouldRetryIndexCreateCmd(cmdObj)) {
                jsTest.log.info("-=-=-=- Retrying after IndexBuildAborted error response",
                                {cmdObj, res});
                continue;
            } else {
                return res;
            }
        } else if (shouldRetryOnInterruptedError(res)) {
            jsTest.log.info("-=-=-=- Retrying after receiving Interrupted Error code",
                            {cmdObj, res});
            continue;
        }

        return res;
    }
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/retry_aborted_db_and_index_creation.js");
