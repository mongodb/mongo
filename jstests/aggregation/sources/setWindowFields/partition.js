/*
 * Test partitioning inside $setWindowFields.
 */
(function() {
"use strict";

const getParam = db.adminCommand({getParameter: 1, featureFlagWindowFunctions: 1});
jsTestLog(getParam);
const featureEnabled = assert.commandWorked(getParam).featureFlagWindowFunctions.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the window function feature flag is disabled");
    return;
}

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insert({int_field: 0, arr: [1, 2]}));

// TODO SERVER-53402 Enable partitionBy tests.
// Test for runtime error when partitionBy expression evaluates to an array
// assert.commandFailedWithCode(
// run({$setWindowFields: {partitionBy: "$arr", output: {}}}),
// ErrorCodes.TypeMismatch);
})();