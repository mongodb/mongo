/**
 * Tests that the query stats store can be resized if it is configured, and cannot be resized if it
 * is disabled.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

if (FeatureFlagUtil.isEnabled(db, "QueryStats") || FeatureFlagUtil.isEnabled(db, "QueryStatsFindCommand")) {
    function testQueryStatsSetting(paramName, paramValue) {
        // The feature flag is enabled - make sure the queryStats store can be configured.
        const original = assert.commandWorked(db.adminCommand({getParameter: 1, [paramName]: 1}));
        assert(original.hasOwnProperty(paramName), original);
        const originalValue = original[paramName];
        try {
            assert.doesNotThrow(() => db.adminCommand({setParameter: 1, [paramName]: paramValue}));
            // Other tests verify that changing the parameter actually affects the behavior.
        } finally {
            assert.doesNotThrow(() => db.adminCommand({setParameter: 1, [paramName]: originalValue}));
        }
    }
    testQueryStatsSetting("internalQueryStatsCacheSize", "2MB");
    testQueryStatsSetting("internalQueryStatsRateLimit", 2147483647);
} else {
    // The feature flag is disabled - make sure the query stats store *cannot* be configured.
    assert.commandFailedWithCode(db.adminCommand({setParameter: 1, internalQueryStatsCacheSize: "2MB"}), 7373500);
    assert.commandFailedWithCode(db.adminCommand({setParameter: 1, internalQueryStatsRateLimit: 2147483647}), 7506200);
}
