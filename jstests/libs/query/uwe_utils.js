import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

/**
 * Checks whether the unified write executor is used for sharded writes.
 */
export function isUweEnabled(db) {
    return FeatureFlagUtil.isPresentAndEnabled(db, "UnifiedWriteExecutor");
}

/**
 * Checks whether the unified write executor uses a different write command name on the shards.
 */
export function isUweShardCmdNameChanged(cmdName) {
    return ["insert", "update", "delete", "remove"].includes(cmdName);
}

/**
 * Maps the write command name that the unified write executor uses on the shards.
 */
export function mapUweShardCmdName(cmdName) {
    if (isUweShardCmdNameChanged(cmdName)) {
        return "bulkWrite";
    }
    return cmdName;
}
