/**
 * This override prevents running commands that use the WiredTiger size storer. Because the size
 * storer is only updated periodically, commands that use it after an unclean shutdown could return
 * inaccurate results.
 */
(function() {
    "use strict";

    load("jstests/libs/override_methods/override_helpers.js");

    function runCommandFailUncleanShutdownIncompatibleCommands(
        conn, dbName, commandName, commandObj, func, makeFuncArgs) {
        if (typeof commandObj !== "object" || commandObj === null) {
            return func.apply(conn, makeFuncArgs(commandObj));
        }

        // If the command is in a wrapped form, then we look for the actual command object inside
        // the query/$query object.
        let commandObjUnwrapped = commandObj;
        if (commandName === "query" || commandName === "$query") {
            commandObjUnwrapped = commandObj[commandName];
            commandName = Object.keys(commandObjUnwrapped)[0];
        }

        if (commandName === "count" && (!commandObjUnwrapped.hasOwnProperty("query") ||
                                        Object.keys(commandObjUnwrapped["query"]).length === 0)) {
            throw new Error("Cowardly fail if fastcount is run with a mongod that had an unclean" +
                            " shutdown: " + tojson(commandObjUnwrapped));
        }

        if (commandName === "dataSize" && !commandObjUnwrapped.hasOwnProperty("min") &&
            !commandObjUnwrapped.hasOwnProperty("max")) {
            throw new Error("Cowardly fail if unbounded dataSize is run with a mongod that had an" +
                            " unclean shutdown: " + tojson(commandObjUnwrapped));
        }

        if (commandName === "collStats" || commandName === "dbStats") {
            throw new Error("Cowardly fail if " + commandName + " is run with a mongod that had" +
                            " an unclean shutdown: " + tojson(commandObjUnwrapped));
        }

        return func.apply(conn, makeFuncArgs(commandObj));
    }

    OverrideHelpers.prependOverrideInParallelShell(
        "jstests/libs/override_methods/fail_unclean_shutdown_incompatible_commands.js");

    OverrideHelpers.overrideRunCommand(runCommandFailUncleanShutdownIncompatibleCommands);
})();
