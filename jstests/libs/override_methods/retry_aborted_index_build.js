/**
 * Overrides Mongo.prototype.runCommand to retry index build commands
 * which resulted in an IndexBuildAborted Error.
 * Was modeled partly on retry_on_killed_session.js.
 */
(function() {
"use strict";

load("jstests/libs/override_methods/override_helpers.js");

const mongoRunCommandOriginal = Mongo.prototype.runCommand;

Mongo.prototype.runCommand = function runCommand(dbName, cmdObj, options) {
    return runWithIndexBuildAbortedRetries(this, cmdObj, mongoRunCommandOriginal, arguments);
};

const kCmds = new Set(["createIndexes", "createIndex"]);
const kMaxRetry = 100;

// Returns if the command should retry on IndexBuildAborted errors.
function shouldRetry(cmdObj) {
    if (cmdObj.hasOwnProperty("autocommit")) {
        // Transactions are retried at a higher level.
        return false;
    }

    const cmdName = Object.keys(cmdObj)[0];
    if (kCmds.has(cmdName)) {
        return true;
    }

    return false;
}

// Returns if the code is one that could come from an index build being aborted.
function isIndexBuildAbortedCode(code) {
    return code === ErrorCodes.IndexBuildAborted;
}

function hasIndexBuildAbortedError(res) {
    return isIndexBuildAbortedCode(res.code);
}

function runWithIndexBuildAbortedRetries(mongo, cmdObj, clientFunction, clientFunctionArguments) {
    if (!shouldRetry(cmdObj)) {
        return clientFunction.apply(mongo, clientFunctionArguments);
    }

    let retryCount = 0;
    while (true) {
        const res = clientFunction.apply(mongo, clientFunctionArguments);

        if (hasIndexBuildAbortedError(res) && retryCount++ < kMaxRetry) {
            print("-=-=-=- Retrying " + tojsononeline(cmdObj) +
                  " after IndexBuildAborted error response: " + tojsononeline(res));
            continue;
        }

        return res;
    }
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/retry_aborted_index_build.js");
})();
