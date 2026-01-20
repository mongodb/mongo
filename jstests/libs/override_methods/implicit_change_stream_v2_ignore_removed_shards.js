/**
 * Overrides the $changeStream aggregation pipeline to run in "ignoreRemovedShards" mode.
 * This requires that the change stream is opened with version "v2".
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

function isChangeStreamCommandV2WithoutIgnoreRemovedShards(cmdObj) {
    return (
        cmdObj &&
        cmdObj.aggregate &&
        // TODO: SERVER-111381 Implement AllDatabasesChangeStreamShardTargeterImpl module.
        cmdObj.aggregate.$db !== "admin" &&
        Array.isArray(cmdObj.pipeline) &&
        cmdObj.pipeline.length > 0 &&
        typeof cmdObj.pipeline[0].$changeStream == "object" &&
        cmdObj.pipeline[0].$changeStream.constructor === Object &&
        cmdObj.pipeline[0].$changeStream.version === "v2" &&
        !cmdObj.pipeline[0].$changeStream.hasOwnProperty("ignoreRemovedShards")
    );
}

function runChangeStreamWithIgnoreRemovedShards(conn, _dbName, _commandName, commandObj, func, makeFuncArgs) {
    // TODO: SERVER-52253 Enable feature flag for Improved change stream handling of cluster topology changes.
    if (isChangeStreamCommandV2WithoutIgnoreRemovedShards(commandObj) && isFeatureFlagEnabled(conn)) {
        commandObj.pipeline[0].$changeStream["ignoreRemovedShards"] = true;
    }
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/implicit_change_stream_v2_ignore_removed_shards.js",
);
OverrideHelpers.overrideRunCommand(runChangeStreamWithIgnoreRemovedShards);
