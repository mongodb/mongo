/**
 * Uses prototype overrides to set the apiVersion field to "1" for each command when running tests.
 */
(function() {
"use strict";

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.

const apiVersion = "1";

function runCommandWithApiVersion(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    if (commandObj.hasOwnProperty("apiVersion") || commandObj.hasOwnProperty("apiStrict") ||
        commandObj.hasOwnProperty("apiDeprecationErrors")) {
        throw new Error(
            `Cowardly refusing to override API parameters of command ${tojson(commandObj)}`);
    }

    // We create a copy of 'commandObj' to avoid mutating the parameter the caller specified.
    commandObj = Object.assign({}, commandObj);

    // Set the API version on the command object.
    commandObj.apiVersion = apiVersion;
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell("jstests/libs/override_methods/set_api_version.js");

OverrideHelpers.overrideRunCommand(runCommandWithApiVersion);
})();
