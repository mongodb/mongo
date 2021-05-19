/**
 * Uses prototype overrides to set the apiVersion field to "1" for each command when running tests.
 */
(function() {
"use strict";

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.

const apiVersion = "1";

function runCommandWithApiVersion(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    // If the command is in a wrapped form, then we look for the actual command object inside
    // the query/$query object.
    let commandObjUnwrapped = commandObj;
    if (commandName === "query" || commandName === "$query") {
        commandObjUnwrapped = commandObj[commandName];
        commandName = Object.keys(commandObjUnwrapped)[0];
    }

    if (commandObjUnwrapped.hasOwnProperty("apiVersion") ||
        commandObjUnwrapped.hasOwnProperty("apiStrict") ||
        commandObjUnwrapped.hasOwnProperty("apiDeprecationErrors")) {
        throw new Error(
            `Cowardly refusing to override API parameters of command ${tojson(commandObj)}`);
    }

    const inWrappedForm = commandObj !== commandObjUnwrapped;

    // We create a copy of 'commandObj' to avoid mutating the parameter the caller
    // specified.
    commandObj = Object.assign({}, commandObj);
    if (inWrappedForm) {
        commandObjUnwrapped = Object.assign({}, commandObjUnwrapped);
        commandObj[Object.keys(commandObj)[0]] = commandObjUnwrapped;
    } else {
        commandObjUnwrapped = commandObj;
    }

    // Set the API version on the command object.
    commandObjUnwrapped.apiVersion = apiVersion;
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell("jstests/libs/override_methods/set_api_version.js");

OverrideHelpers.overrideRunCommand(runCommandWithApiVersion);
})();
