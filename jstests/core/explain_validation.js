/**
 * Tests that an unknown field in the explain command will be rejected while generic command
 * arguments will be permitted to pass validation.
 *
 * @tags: [
 *   requires_fcv_47,
 * ]
 */

(function() {
"use strict";

const testDB = db.getSiblingDB(jsTestName());

// Drop and recreate the test database and a test collection.
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.test.createIndex({a: 1}));

// Test that an unknown field is rejected during explain command parsing.
assert.commandFailedWithCode(testDB.runCommand({explain: {find: "test"}, unknownField: true}),
                             40415);

// Test that a generic argument will be accepted by command parsing.
assert.commandWorked(testDB.runCommand({explain: {find: "test"}, comment: true}));
})();
