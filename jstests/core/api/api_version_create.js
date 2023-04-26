/**
 * Tests the create command with API versioning enabled.
 *
 * @tags: [
 *   uses_api_parameters,
 * ]
 */
(function() {
"use strict";

const testDB = db.getSiblingDB("createAPIVersion");
testDB.dropDatabase();

// Test the command with 'apiStrict'.
assert.commandWorked(testDB.runCommand({create: "basicCreate", apiVersion: "1", apiStrict: true}));

// Test the command with 'apiStrict'.
assert.commandWorked(testDB.runCommand(
    {create: "basicCreateCappedFalse", capped: false, apiVersion: "1", apiStrict: true}));

// Test that creating a capped collection fails with apiStrict=true.
assert.commandFailedWithCode(testDB.runCommand({
    create: "withCappedTrue",
    capped: true,
    size: 1000,
    apiVersion: "1",
    apiStrict: true,
}),
                             ErrorCodes.APIStrictError);

// Test that creating a capped collection fails without the size parameter.
assert.commandFailedWithCode(testDB.runCommand({
    create: "withCappedTrueNoSize",
    capped: true,
    apiVersion: "1",
    apiStrict: true,
}),
                             ErrorCodes.InvalidOptions);
})();
