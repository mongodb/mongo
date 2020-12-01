/**
 * Test the syntax of $setWindowFields. For example:
 * - Which options are allowed?
 * - When is an expression expected, vs a constant?
 */
(function() {
"use strict";

const featureEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagWindowFunctions: 1}))
        .featureFlagWindowFunctions.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the window function feature flag is disabled");
    return;
}

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert({}));

function run(stage, extraCommandArgs = {}) {
    return coll.runCommand(
        Object.merge({aggregate: coll.getName(), pipeline: [stage], cursor: {}}, extraCommandArgs));
}

// Test that the stage spec must be an object.
assert.commandFailedWithCode(run({$setWindowFields: "invalid"}), ErrorCodes.FailedToParse);

// Test that the stage parameters are the correct type.
assert.commandFailedWithCode(run({$setWindowFields: {sortBy: "invalid"}}), ErrorCodes.TypeMismatch);
assert.commandFailedWithCode(run({$setWindowFields: {output: "invalid"}}), ErrorCodes.TypeMismatch);

// Test that parsing fails for an invalid partitionBy expression.
assert.commandFailedWithCode(
    run({$setWindowFields: {partitionBy: {$notAnOperator: 1}, output: {}}}),
    ErrorCodes.InvalidPipelineOperator);

// Since partitionBy can be any expression, it can be a variable.
assert.commandWorked(run({$setWindowFields: {partitionBy: "$$NOW", output: {}}}));
assert.commandWorked(
    run({$setWindowFields: {partitionBy: "$$myobj.a", output: {}}}, {let : {myobj: {a: 456}}}));

// Test that parsing fails for unrecognized parameters.
assert.commandFailedWithCode(run({$setWindowFields: {what_is_this: 1}}), 40415);

// Test for a successful parse, ignoring the response documents.
assert.commandWorked(
    run({$setWindowFields: {partitionBy: "$state", sortBy: {city: 1}, output: {a: {$sum: 1}}}}));
})();
