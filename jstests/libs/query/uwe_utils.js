import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

/**
 * Checks whether the unified write executor is used for sharded writes.
 */
export function isUweEnabled(db) {
    return FeatureFlagUtil.isPresentAndEnabled(db, "UnifiedWriteExecutor");
}
