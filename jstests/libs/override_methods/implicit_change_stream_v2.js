/**
 * Overrides the $changeStream aggregation pipeline to run in version "v2".
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

let featureFlagEnabled = null;
function isFeatureFlagEnabled(conn) {
    if (featureFlagEnabled === null) {
        featureFlagEnabled = FeatureFlagUtil.isPresentAndEnabled(conn, "ChangeStreamPreciseShardTargeting");
    }
    return featureFlagEnabled;
}

function isChangeStreamCommandWithoutVersion(cmdObj) {
    return (
        cmdObj &&
        cmdObj.aggregate &&
        // TODO: SERVER-111325 Implement DatabaseChangeStreamShardTargeterImpl module.
        cmdObj.aggregate !== 1 &&
        // TODO: SERVER-111381 Implement AllDatabasesChangeStreamShardTargeterImpl module.
        cmdObj.aggregate.$db !== "admin" &&
        Array.isArray(cmdObj.pipeline) &&
        cmdObj.pipeline.length > 0 &&
        typeof cmdObj.pipeline[0].$changeStream == "object" &&
        cmdObj.pipeline[0].$changeStream.constructor === Object &&
        !cmdObj.pipeline[0].$changeStream.hasOwnProperty("version")
    );
}

function runChangeStreamWithV2Version(conn, _dbName, _commandName, commandObj, func, makeFuncArgs) {
    // TODO: SERVER-52253 Enable feature flag for Improved change stream handling of cluster topology changes.
    if (isChangeStreamCommandWithoutVersion(commandObj) && isFeatureFlagEnabled(conn)) {
        commandObj.pipeline[0].$changeStream["version"] = "v2";
    }
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell("jstests/libs/override_methods/implicit_change_stream_v2.js");
OverrideHelpers.overrideRunCommand(runChangeStreamWithV2Version);
