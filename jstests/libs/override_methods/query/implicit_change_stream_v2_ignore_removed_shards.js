/**
 * Overrides the $changeStream aggregation pipeline to run in "ignoreRemovedShards" mode.
 * This requires that the change stream is opened with version "v2".
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

function isChangeStreamCommandV2WithoutIgnoreRemovedShards(cmdObj) {
    if (
        !cmdObj ||
        !cmdObj.aggregate ||
        !Array.isArray(cmdObj.pipeline) ||
        cmdObj.pipeline.length === 0 ||
        typeof cmdObj.pipeline[0].$changeStream != "object" ||
        cmdObj.pipeline[0].$changeStream.constructor !== Object
    ) {
        return false;
    }

    // The stream must already be v2, either explicitly or implicitly (no version specified defaults to v2).
    const changeStreamSpec = cmdObj.pipeline[0].$changeStream;
    const isV2 = changeStreamSpec.version === "v2" || !changeStreamSpec.hasOwnProperty("version");
    return isV2 && !changeStreamSpec.hasOwnProperty("ignoreRemovedShards");
}

function runChangeStreamWithIgnoreRemovedShards(
    conn,
    _dbName,
    _commandName,
    commandObj,
    func,
    makeFuncArgs,
) {
    if (isChangeStreamCommandV2WithoutIgnoreRemovedShards(commandObj)) {
        commandObj.pipeline[0].$changeStream["ignoreRemovedShards"] = true;
    }
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/query/implicit_change_stream_v2_ignore_removed_shards.js",
);
OverrideHelpers.overrideRunCommand(runChangeStreamWithIgnoreRemovedShards);
