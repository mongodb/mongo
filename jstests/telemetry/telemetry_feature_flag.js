/**
 * Test that calls to read from telemetry store fail when feature flag is turned off.
 */
load('jstests/libs/analyze_plan.js');
load("jstests/libs/feature_flag_util.js");

(function() {
"use strict";

// This test specifically tests error handling when the feature flag is not on.
if (FeatureFlagUtil.isEnabled(db, "Telemetry")) {
    return;
}

// Pipeline to read telemetry store should fail without feature flag turned on.
assert.commandFailedWithCode(
    db.adminCommand({aggregate: 1, pipeline: [{$telemetry: {}}], cursor: {}}),
    ErrorCodes.QueryFeatureNotAllowed);

// Pipeline, with a filter, to read telemetry store fails without feature flag turned on.
assert.commandFailedWithCode(db.adminCommand({
    aggregate: 1,
    pipeline: [{$telemetry: {}}, {$match: {"key.find.find": {$eq: "###"}}}],
    cursor: {}
}),
                             ErrorCodes.QueryFeatureNotAllowed);
}());
