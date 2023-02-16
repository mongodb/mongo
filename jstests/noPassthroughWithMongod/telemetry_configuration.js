/**
 * Tests that the telemetry store can be resized if it is configured, and cannot be resized if it is
 * disabled.
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");

if (FeatureFlagUtil.isEnabled(db, "Telemetry")) {
    // The feature flag is enabled - make sure the telemetry store can be configured.
    const original = assert.commandWorked(
        db.adminCommand({getParameter: 1, internalQueryConfigureTelemetryCacheSize: 1}));
    assert(original.hasOwnProperty("internalQueryConfigureTelemetryCacheSize"), original);
    const originalValue = original.internalQueryConfigureTelemetryCacheSize;
    try {
        assert.doesNotThrow(
            () => db.adminCommand(
                {setParameter: 1, internalQueryConfigureTelemetryCacheSize: '2MB'}));
        // Other tests verify that resizing actually affects the data structure size.
    } finally {
        assert.doesNotThrow(
            () => db.adminCommand(
                {setParameter: 1, internalQueryConfigureTelemetryCacheSize: originalValue}));
    }
} else {
    // The feature flag is disabled - make sure the telemetry store *cannot* be configured.
    assert.commandFailedWithCode(
        db.adminCommand({setParameter: 1, internalQueryConfigureTelemetryCacheSize: '2MB'}),
        7373500);
}
}());
