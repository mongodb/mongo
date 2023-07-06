/**
 * Loading this file overrides DB.prototype._runCommandImpl with a function that adds to any
 * $changeStream a filter on operationType that excludes all endOfTransaction events. This is needed
 * for multistatement transaction test suites that are not expect to have transactions by default.
 * Non-$changeStream commands and commands which explicitly request to be exempted from modification
 * by setting the 'noPassthrough' flag, are passed through as-is.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const endOfTransactionFilter = {
    $match: {operationType: {$ne: "endOfTransaction"}}
};

const runCommandWithPassthroughEotFilter = function(
    conn, _dbName, _commandName, commandObj, func, makeFuncArgs) {
    if (OverrideHelpers.isAggregationWithChangeStreamStage(_commandName, commandObj)) {
        const newPipeline = Object.assign([], commandObj.pipeline);
        newPipeline.splice(1, 0, endOfTransactionFilter);
        commandObj.pipeline = newPipeline;
    }
    return func.apply(conn, makeFuncArgs(commandObj));
};

OverrideHelpers.overrideRunCommand(runCommandWithPassthroughEotFilter);
