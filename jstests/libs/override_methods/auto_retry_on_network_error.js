/**
 * When a network connection to the mongo shell is closed, attempting to call
 * Mongo.prototype.runCommand() and Mongo.prototype.runCommandWithMetadata() throws a JavaScript
 * exception. This override catches these exceptions (i.e. ones where isNetworkError() returns true)
 * and automatically re-sends the command request to the server, or propagates the error if the
 * command should already be using the shell's existing retryability logic. The goal of this
 * override is to implement retry logic such that the assertions within our existing JavaScript
 * tests still pass despite stepdowns of the CSRS and replica set shards happening in the
 * background.
 */
(function() {
    "use strict";

    const retryableWriteCommands =
        new Set(["delete", "findandmodify", "findAndModify", "insert", "update"]);

    // Store a session to access ServerSession#canRetryWrites.
    let _serverSession;

    const mongoRunCommandOriginal = Mongo.prototype.runCommand;
    const mongoRunCommandWithMetadataOriginal = Mongo.prototype.runCommandWithMetadata;

    Mongo.prototype.runCommand = function runCommand(dbName, cmdObj, options) {
        if (typeof _serverSession === "undefined") {
            _serverSession = this.startSession()._serverSession;
        }

        return runWithRetriesOnNetworkErrors(this, cmdObj, mongoRunCommandOriginal, arguments);
    };

    Mongo.prototype.runCommandWithMetadata = function runCommandWithMetadata(
        dbName, metadata, cmdObj) {
        if (typeof _serverSession === "undefined") {
            _serverSession = this.startSession()._serverSession;
        }

        return runWithRetriesOnNetworkErrors(
            this, cmdObj, mongoRunCommandWithMetadataOriginal, arguments);
    };

    function runWithRetriesOnNetworkErrors(mongo, cmdObj, clientFunction, clientFunctionArguments) {
        let cmdName = Object.keys(cmdObj)[0];

        // If the command is in a wrapped form, then we look for the actual command object
        // inside the query/$query object.
        if (cmdName === "query" || cmdName === "$query") {
            cmdObj = cmdObj[cmdName];
            cmdName = Object.keys(cmdObj)[0];
        }

        const isRetryableWriteCmd = retryableWriteCommands.has(cmdName);
        const canRetryWrites = _serverSession.canRetryWrites(cmdObj);

        let numRetries = !jsTest.options().skipRetryOnNetworkError ? 1 : 0;

        do {
            try {
                return clientFunction.apply(mongo, clientFunctionArguments);
            } catch (e) {
                if (!isNetworkError(e) || numRetries === 0) {
                    throw e;
                } else if (isRetryableWriteCmd) {
                    if (canRetryWrites) {
                        // If the command is retryable, assume the command has already gone through
                        // or will go through the retry logic in SessionAwareClient, so propagate
                        // the error.
                        throw e;
                    } else {
                        throw new Error(
                            "Cowardly refusing to run a test that issues non-retryable write" +
                            " operations since the test likely makes assertions on the write" +
                            " results and can lead to spurious failures if a network error" +
                            " occurs.");
                    }
                } else if (cmdName === "getMore") {
                    throw new Error(
                        "Cowardly refusing to run a test that issues a getMore command since if" +
                        " a network error occurs during it then we won't know whether the cursor" +
                        " was advanced or not.");
                }

                --numRetries;
            }
        } while (numRetries >= 0);
    }

    const startParallelShellOriginal = startParallelShell;

    startParallelShell = function(jsCode, port, noConnect) {
        let newCode;
        const overridesFile = "jstests/libs/override_methods/auto_retry_on_network_error.js";
        if (typeof(jsCode) === "function") {
            // Load the override file and immediately invoke the supplied function.
            newCode = `load("${overridesFile}"); (${jsCode})();`;
        } else {
            newCode = `load("${overridesFile}"); ${jsCode};`;
        }

        return startParallelShellOriginal(newCode, port, noConnect);
    };
})();
