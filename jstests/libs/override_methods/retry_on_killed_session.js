/**
 * Overrides Mongo.prototype.runCommand to retry on errors that come from an operation's session
 * being killed.
 */
(function() {
"use strict";

load("jstests/libs/killed_session_util.js");
load("jstests/libs/override_methods/override_helpers.js");

const mongoRunCommandOriginal = Mongo.prototype.runCommand;

Mongo.prototype.runCommand = function runCommand(dbName, cmdObj, options) {
    return runWithKilledSessionRetries(this, cmdObj, mongoRunCommandOriginal, arguments);
};

// Returns if the command should retry on killed session errors.
function shouldRetry(cmdObj) {
    if (cmdObj.hasOwnProperty("autocommit")) {
        // Transactions are retried at a higher level.
        return false;
    }

    // Assume every other operation can be retried. Callers should guarantee only idempotent
    // operations are run when this override is active.
    return true;
}

function runWithKilledSessionRetries(mongo, cmdObj, clientFunction, clientFunctionArguments) {
    if (!shouldRetry(cmdObj)) {
        return clientFunction.apply(mongo, clientFunctionArguments);
    }

    while (true) {
        try {
            const res = clientFunction.apply(mongo, clientFunctionArguments);

            if (KilledSessionUtil.hasKilledSessionError(res)) {
                print("-=-=-=- Retrying " + tojsononeline(cmdObj) +
                      " after killed session error response: " + tojsononeline(res));
                continue;
            }

            if (KilledSessionUtil.hasKilledSessionWCError(res)) {
                print("-=-=-=- Retrying " + tojsononeline(cmdObj) +
                      " after killed session write concern error response: " + tojsononeline(res));
                continue;
            }

            return res;
        } catch (e) {
            if (KilledSessionUtil.hasKilledSessionError(e)) {
                print("-=-=-=- Retrying " + tojsononeline(cmdObj) +
                      " after thrown killed session error: " + tojsononeline(e));
                continue;
            }
            throw e;
        }
    }
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/retry_on_killed_session.js");
})();
