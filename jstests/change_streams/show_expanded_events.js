/**
 * Tests the behavior of change streams in the presence of 'showExpandedEvents' flag.
 *
 * @tags: [ requires_fcv_53, ]
 */
(function() {
"use strict";

const isFeatureEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagChangeStreamsVisibility: 1}))
        .featureFlagChangeStreamsVisibility.value;
if (!isFeatureEnabled) {
    assert.commandFailedWithCode(db.runCommand({
        aggregate: 1,
        pipeline: [{$changeStream: {showExpandedEvents: true}}],
        cursor: {},
    }),
                                 6188501);
    return;
}

// Assert that the flag is not allowed with 'apiStrict'.
assert.commandFailedWithCode(db.runCommand({
    aggregate: 1,
    pipeline: [{$changeStream: {showExpandedEvents: true}}],
    cursor: {},
    apiVersion: "1",
    apiStrict: true
}),
                             ErrorCodes.APIStrictError);

assert.commandWorked(db.runCommand(
    {aggregate: 1, pipeline: [{$changeStream: {showExpandedEvents: true}}], cursor: {}}));
}());