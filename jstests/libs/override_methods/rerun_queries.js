/**
 * Overrides Mongo.prototype.runCommand for read-only commands in order to run them multiple times
 * in a row and return the result from the final execution. It is intended to help with testing any
 * caches that might be involved in query processing, in particular the plan cache.
 */
(function() {
"use strict";

load("jstests/libs/override_methods/override_helpers.js");

// Represents the number of repeated runs of the command. The number is set to 3, as at first run we
// will create a plan cache entry, at the second run we will promote the cache entry to active and
// at the third run we will use the active cache entry.
const numberOfRuns = 3;
const kRerunnableReadCommands =
    new Set(["aggregate", "find", "count", "distinct", "mapReduce", "mapreduce"]);

function isRerunnableQuery(cmdName, cmdObj) {
    if (OverrideHelpers.isAggregationWithOutOrMergeStage(cmdName, cmdObj) ||
        (cmdName.toLowerCase() == "mapreduce" &&
         !OverrideHelpers.isMapReduceWithInlineOutput(cmdName, cmdObj))) {
        return false;
    }

    return kRerunnableReadCommands.has(cmdName);
}

function runCommandOverride(conn, dbName, cmdName, cmdObj, clientFunction, makeFuncArgs) {
    if (!isRerunnableQuery(cmdName, cmdObj)) {
        return clientFunction.apply(conn, makeFuncArgs(cmdObj));
    }

    let lastResult;
    for (let run = 0; run < numberOfRuns; run++) {
        lastResult = clientFunction.apply(conn, makeFuncArgs(cmdObj));
    }

    return lastResult;
}

// Override the default runCommand with our custom version.
OverrideHelpers.overrideRunCommand(runCommandOverride);

// Always apply the override if a test spawns a parallel shell.
OverrideHelpers.prependOverrideInParallelShell("jstests/libs/override_methods/rerun_queries.js");
})();
