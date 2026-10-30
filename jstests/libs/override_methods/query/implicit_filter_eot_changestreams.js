/**
 * Loading this file overrides runCommand with a function that adds to any $changeStream a filter on
 * operationType that excludes all endOfTransaction events. This is needed for multistatement
 * transaction test suites that are not expected to have transactions by default. The filter is
 * injected both when the change stream is executed directly and when it is nested inside an
 * 'explain', so that explaining a change stream yields the same query shape (and query shape hash)
 * as running it. Non-$changeStream commands are passed through as-is.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const endOfTransactionFilter = {
    $match: {operationType: {$ne: "endOfTransaction"}},
};

// Returns the $changeStream aggregate command nested (either directly or inside an 'explain') in
// 'cmdObj' if the endOfTransaction filter should be injected, and null otherwise.
function getChangeStreamAggregate(cmdObj) {
    if (!cmdObj) {
        return null;
    }
    const aggCmd = cmdObj.aggregate
        ? cmdObj
        : typeof cmdObj.explain === "object"
          ? cmdObj.explain
          : null;
    return OverrideHelpers.isAggregationWithChangeStreamStage("aggregate", aggCmd) ? aggCmd : null;
}

const runCommandWithPassthroughEotFilter = function (
    conn,
    _dbName,
    _commandName,
    commandObj,
    func,
    makeFuncArgs,
) {
    const aggCmd = getChangeStreamAggregate(commandObj);
    if (aggCmd) {
        const newPipeline = Object.assign([], aggCmd.pipeline);
        newPipeline.splice(1, 0, endOfTransactionFilter);
        aggCmd.pipeline = newPipeline;
    }
    return func.apply(conn, makeFuncArgs(commandObj));
};

OverrideHelpers.overrideRunCommand(runCommandWithPassthroughEotFilter);
