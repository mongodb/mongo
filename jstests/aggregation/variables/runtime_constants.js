/*
 * Tests the behavior of runtime constants $$IS_MR and $$JS_SCOPE.
 */

import "jstests/libs/sbe_assert_error_override.js";

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
