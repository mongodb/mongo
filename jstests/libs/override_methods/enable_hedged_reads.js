/**
 * Use prototype overrides to set read preference to enable hedge reads when running tests.
 */
(function() {
"use strict";

load("jstests/libs/override_methods/override_helpers.js");

const kReadPreferenceNearest = {
    mode: "nearest"
};
const kReadPreferencePrimary = {
    mode: "primary"
};
const kCommandsSupportingReadPreference = new Set([
    "aggregate",
    "collStats",
    "count",
    "dbStats",
    "distinct",
    "find",
]);
const kDatabasesOnConfigServers = new Set(["config", "admin"]);

function runCommandWithHedgedReads(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    if (typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    // The profile collection is not replicated
    if (commandObj[commandName] === "system.profile" || commandName === 'profile') {
        throw new Error(
            "Cowardly refusing to run test that interacts with the system profiler as the " +
            "'system.profile' collection is not replicated" + tojson(commandObj));
    }

    let shouldForceReadPreference = kCommandsSupportingReadPreference.has(commandName);
    if (OverrideHelpers.isAggregationWithOutOrMergeStage(commandName, commandObj)) {
        // An aggregation with a $out stage must be sent to the primary.
        shouldForceReadPreference = false;
    } else if ((commandName === "mapReduce" || commandName === "mapreduce") &&
               !OverrideHelpers.isMapReduceWithInlineOutput(commandName, commandObj)) {
        // A map-reduce operation with non-inline output must be sent to the primary.
        shouldForceReadPreference = false;
    } else if (!conn.isMongos()) {
        shouldForceReadPreference = false;
    } else if (conn.isMongos() && kDatabasesOnConfigServers.has(dbName)) {
        // Avoid overriding the read preference for config server since there may only be one
        // of them.
        shouldForceReadPreference = false;
    }

    if (TestData.doNotOverrideReadPreference) {
        // Use this TestData flag to allow certain runCommands to be exempted from
        // setting secondary read preference.
        shouldForceReadPreference = false;
    }

    if (shouldForceReadPreference) {
        if (commandObj.hasOwnProperty("$readPreference")) {
            if (commandObj.$readPreference.hasOwnProperty("hedge") &&
                bsonBinaryEqual({_: commandObj.$readPreference.hedge}, {_: {enabled: false}})) {
                throw new Error("Cowardly refusing to override read preference of command: " +
                                tojson(commandObj));
            }
            if (bsonBinaryEqual({_: commandObj.$readPreference}, {_: kReadPreferencePrimary})) {
                throw new Error("Cowardly refusing to override read preference of command: " +
                                tojson(commandObj));
            } else if (!bsonBinaryEqual({_: commandObj.$readPreference},
                                        {_: kReadPreferenceNearest})) {
                if (!commandObj.$readPreference.hasOwnProperty("hedge")) {
                    commandObj.$readPreference.hedge = {enabled: true};
                }
            }
        } else {
            commandObj.$readPreference = kReadPreferenceNearest;
        }
    }

    const serverResponse = func.apply(conn, makeFuncArgs(commandObj));

    return serverResponse;
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/enable_hedged_reads.js");

OverrideHelpers.overrideRunCommand(runCommandWithHedgedReads);
})();
