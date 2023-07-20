/*
 * Tests the behavior of runtime constants $$IS_MR and $$JS_SCOPE.
 */

(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

const coll = db.runtime_constants;
coll.drop();

assert.commandWorked(coll.insert({x: true}));

// Runtime constant $$IS_MR is unable to be retrieved by users.
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: coll.getName(), pipeline: [{$addFields: {testField: "$$IS_MR"}}], cursor: {}}),
    [51144]);

// Runtime constant $$JS_SCOPE is unable to be retrieved by users.
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: coll.getName(), pipeline: [{$addFields: {field: "$$JS_SCOPE"}}], cursor: {}}),
    [51144]);
})();
