// Confirm correctness of $add evaluation in find projection.
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
load("jstests/libs/sbe_util.js");             // For checkSBEEnabled.

const isSBEEnabled = checkSBEEnabled(db);
if (isSBEEnabled) {
    // Override error-code-checking APIs. We only load this when SBE is explicitly enabled, because
    // it causes failures in the parallel suites.
    load("jstests/libs/sbe_assert_error_override.js");
}

const coll = db.expression_add;
coll.drop();
assert.commandWorked(coll.insert({a: NumberInt(2), b: NumberLong(3), c: 3.5}));

const testCases = [
    [["$a", "$b", "$c"], 8.5],
    [["$a", "$b", null], null],
    [["$a", "$b", 5], 10],
    [[5, "$a", "$b"], 10],
    [["$a", 5, "$b"], 10],
    [["$a", 20, "$c", 10], 35.5],
];
for (const testCase of testCases) {
    const [addExpr, expected] = testCase;
    assert.eq(coll.findOne({}, {sum: {$add: addExpr}, _id: 0}), {sum: expected}, testCase);
}
})();
