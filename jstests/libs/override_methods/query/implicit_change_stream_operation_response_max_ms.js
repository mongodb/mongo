/**
 * Loading this file overrides runCommand with a function that injects a predefined
 * 'operationResponseMaxMS' query knob value into any aggregate command that opens a $changeStream.
 * This exercises the per-query override of the change stream response deadline across the whole
 * change streams test suite.
 *
 * The knob is injected via the top-level 'querySettings' field of the aggregate command. Commands
 * that already set 'querySettings.queryKnobs.operationResponseMaxMS' are passed through untouched,
 * so tests that choose their own deadline (for example the response deadline tests) keep working.
 * Non-$changeStream commands are passed through as-is.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

// The response deadline, in milliseconds, injected into every $changeStream aggregate that does not
// set one explicitly.
const kOperationResponseMaxMS = NumberLong(250);

function runCommandOverride(conn, _dbName, commandName, cmdObj, func, makeFuncArgs) {
    if (
        commandName !== "aggregate" ||
        !OverrideHelpers.isAggregationWithChangeStreamStage(commandName, cmdObj)
    ) {
        return func.apply(conn, makeFuncArgs(cmdObj));
    }

    // Respect a deadline the command already asks for.
    if (cmdObj.querySettings?.queryKnobs?.operationResponseMaxMS !== undefined) {
        return func.apply(conn, makeFuncArgs(cmdObj));
    }

    // Preserve any other query settings the command already carries and only add the knob.
    const querySettings = Object.assign({}, cmdObj.querySettings);
    querySettings.queryKnobs = Object.assign({}, querySettings.queryKnobs, {
        operationResponseMaxMS: kOperationResponseMaxMS,
    });

    cmdObj = Object.assign({}, cmdObj, {querySettings});
    return func.apply(conn, makeFuncArgs(cmdObj));
}

// Override the default runCommand with our custom version.
OverrideHelpers.overrideRunCommand(runCommandOverride);

// Always apply the override if a test spawns a parallel shell.
OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/query/implicit_change_stream_operation_response_max_ms.js",
);
