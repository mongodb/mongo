/**
 * Overrides commands that are incompatible to run when the config fuzzer is enabled.
 */
(function() {
"use strict";

load("jstests/libs/override_methods/override_helpers.js");

function runCommandOverride(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    if (commandName == "compact") {
        // A single compact can perform up to 10-20 checkpoints - which can cause tests to time out
        // when mongod configurations stress the server.
        throw new Error(
            "Cowardly refusing to run test that uses command 'compact' with the config fuzzer enabled. " +
            tojson(commandObj));
    }

    const serverResponse = func.apply(conn, makeFuncArgs(commandObj));
    return serverResponse;
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/config_fuzzer_incompatible_commands.js");

OverrideHelpers.overrideRunCommand(runCommandOverride);
})();
