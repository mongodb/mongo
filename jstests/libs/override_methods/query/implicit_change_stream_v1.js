/**
 * Overrides the $changeStream aggregation pipeline to run in version "v1".
 */
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

// Returns the nested aggregate command if `cmdObj` is a `$changeStream` aggregate (either directly
// or wrapped in an `explain`), and the $changeStream stage does not already specify a version.
// Otherwise returns null.
function getChangeStreamAggregateWithoutVersion(cmdObj) {
    if (!cmdObj) {
        return null;
    }
    const aggCmd = cmdObj.aggregate
        ? cmdObj
        : typeof cmdObj.explain === "object"
          ? cmdObj.explain
          : null;
    if (
        aggCmd &&
        aggCmd.aggregate &&
        Array.isArray(aggCmd.pipeline) &&
        aggCmd.pipeline.length > 0 &&
        typeof aggCmd.pipeline[0].$changeStream === "object" &&
        aggCmd.pipeline[0].$changeStream.constructor === Object &&
        !aggCmd.pipeline[0].$changeStream.hasOwnProperty("version")
    ) {
        return aggCmd;
    }
    return null;
}

function runChangeStreamWithV1Version(conn, _dbName, _commandName, commandObj, func, makeFuncArgs) {
    const aggCmd = getChangeStreamAggregateWithoutVersion(commandObj);
    if (aggCmd) {
        aggCmd.pipeline[0].$changeStream["version"] = "v1";
    }
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/query/implicit_change_stream_v1.js",
);
OverrideHelpers.overrideRunCommand(runChangeStreamWithV1Version);
