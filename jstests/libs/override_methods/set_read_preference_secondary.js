/**
 * Use prototype overrides to set read preference to "secondary" when running tests.
 */
(function() {
    "use strict";

    load("jstests/libs/override_methods/override_helpers.js");

    const kReadPreferenceSecondary = {
        mode: "secondary"
    };
    const kCommandsSupportingReadPreference = new Set([
        "aggregate",
        "collStats",
        "count",
        "dbStats",
        "distinct",
        "find",
        "geoNear",
        "geoSearch",
        "group",
        "mapReduce",
        "mapreduce",
        "parallelCollectionScan",
    ]);

    function runCommandWithReadPreferenceSecondary(
        conn, dbName, commandName, commandObj, func, makeFuncArgs) {
        if (typeof commandObj !== "object" || commandObj === null) {
            return func.apply(conn, makeFuncArgs(commandObj));
        }

        // If the command is in a wrapped form, then we look for the actual command object inside
        // the query/$query object.
        var commandObjUnwrapped = commandObj;
        if (commandName === "query" || commandName === "$query") {
            commandObjUnwrapped = commandObj[commandName];
            commandName = Object.keys(commandObjUnwrapped)[0];
        }

        if (commandObj[commandName] === "system.profile") {
            throw new Error("Cowardly refusing to run test with overridden read preference" +
                            " when it reads from a non-replicated collection: " +
                            tojson(commandObj));
        }

        var shouldForceReadPreference = kCommandsSupportingReadPreference.has(commandName);
        if (OverrideHelpers.isAggregationWithOutStage(commandName, commandObjUnwrapped)) {
            // An aggregation with a $out stage must be sent to the primary.
            shouldForceReadPreference = false;
        } else if ((commandName === "mapReduce" || commandName === "mapreduce") &&
                   !OverrideHelpers.isMapReduceWithInlineOutput(commandName, commandObjUnwrapped)) {
            // A map-reduce operation with non-inline output must be sent to the primary.
            shouldForceReadPreference = false;
        }

        if (shouldForceReadPreference) {
            if (commandObj === commandObjUnwrapped) {
                // We wrap the command object using a "query" field rather than a "$query" field to
                // match the implementation of DB.prototype._attachReadPreferenceToCommand().
                commandObj = {
                    query: commandObj
                };
            } else {
                // We create a copy of 'commandObj' to avoid mutating the parameter the caller
                // specified.
                commandObj = Object.assign({}, commandObj);
            }

            if (commandObj.hasOwnProperty("$readPreference") &&
                !bsonBinaryEqual({_: commandObj.$readPreference}, {_: kReadPreferenceSecondary})) {
                throw new Error("Cowardly refusing to override read preference of command: " +
                                tojson(commandObj));
            }

            commandObj.$readPreference = kReadPreferenceSecondary;
        }

        return func.apply(conn, makeFuncArgs(commandObj));
    }

    startParallelShell = () => {throw new Error(
        "Cowardly refusing to a run a test that starts a parallel shell because prior to" +
        " MongoDB 3.4 replica set connections couldn't be used in it.");
    };

    OverrideHelpers.overrideRunCommand(runCommandWithReadPreferenceSecondary);
})();
