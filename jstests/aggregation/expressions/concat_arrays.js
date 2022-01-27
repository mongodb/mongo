// Confirm correctness of $concatArrays expression evaluation.

// When SBE is enabled, we expect that each $concatArrays expression will be pushed down into the
// query layer. This does not happen when we wrap aggregations in facets, so we prevent this
// test from running in the 'aggregation_facet_unwind_passthrough' suite.
// @tags: [
//   do_not_wrap_aggregations_in_facets,
// ]
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");        // For assertArrayEq.
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.
load("jstests/libs/sbe_util.js");                   // For checkSBEEnabled.

const coll = db.projection_expr_concat_arrays;
coll.drop();

assert.commandWorked(coll.insertOne({
    int_arr: [1, 2, 3, 4],
    dbl_arr: [10.0, 20.1, 20.4, 50.5],
    nested_arr: [["an", "array"], "arr", [[], [[], "a", "b"]]],
    str_arr: ["a", "b", "c"],
    obj_arr: [{a: 1, b: 2}, {c: 3}, {d: 4, e: 5}],
    null_arr: [null, null, null],
    one_null_arr: [null],
    one_str_arr: ["one"],
    empty_arr: [],
    null_val: null,
    str_val: "a string",
    dbl_val: 2.0,
    int_val: 1,
    obj_val: {a: 1, b: "two"}
}));

function runAndAssert(operands, expectedResult) {
    assertArrayEq({
        actual: coll.aggregate([{$project: {f: {$concatArrays: operands}}}]).map(doc => doc.f),
        expected: expectedResult
    });
}

function runAndAssertNull(operands) {
    runAndAssert(operands, [null]);
}

function runAndAssertThrows(operands) {
    const error =
        assert.throws(() => coll.aggregate([{$project: {f: {$concatArrays: operands}}}]).toArray());
    assert.commandFailedWithCode(error, 28664);
}

runAndAssert(["$int_arr"], [[1, 2, 3, 4]]);
runAndAssert([[0], "$int_arr", [5, 6, 7]], [[0, 1, 2, 3, 4, 5, 6, 7]]);
runAndAssert(["$int_arr", "$str_arr"], [[1, 2, 3, 4, "a", "b", "c"]]);
runAndAssert(
    ["$obj_arr", "$obj_arr", "$null_arr"],
    [[{a: 1, b: 2}, {c: 3}, {d: 4, e: 5}, {a: 1, b: 2}, {c: 3}, {d: 4, e: 5}, null, null, null]]);
runAndAssert(["$int_arr", "$str_arr", "$nested_arr"],
             [[1, 2, 3, 4, "a", "b", "c", ["an", "array"], "arr", [[], [[], "a", "b"]]]]);
runAndAssert(["$int_arr", "$obj_arr"], [[1, 2, 3, 4, {a: 1, b: 2}, {c: 3}, {d: 4, e: 5}]]);
runAndAssert(["$obj_arr"], [[{a: 1, b: 2}, {c: 3}, {d: 4, e: 5}]]);
runAndAssert(["$obj_arr", [{o: 123, b: 1}, {y: "o", d: "a"}]],
             [[{a: 1, b: 2}, {c: 3}, {d: 4, e: 5}, {o: 123, b: 1}, {y: "o", d: "a"}]]);

// Confirm that arrays containing null can be concatenated.
runAndAssert(["$null_arr"], [[null, null, null]]);
runAndAssert([[null], "$null_arr"], [[null, null, null, null]]);
runAndAssert("$one_null_arr", [[null]]);
runAndAssert(["$null_arr", "$one_null_arr", "$int_arr", "$null_arr"],
             [[null, null, null, null, 1, 2, 3, 4, null, null, null]]);

// Test operands that form more complex expressions.
runAndAssert([{$concatArrays: "$int_arr"}], [[1, 2, 3, 4]]);
runAndAssert([{$concatArrays: "$int_arr"}, {$concatArrays: {$concatArrays: "$str_arr"}}],
             [[1, 2, 3, 4, "a", "b", "c"]]);
runAndAssert(["$str_arr", {$filter: {input: "$int_arr",
                         as: "num",
                         cond: { $and: [
                                    { $gte: [ "$$num", 2 ] },
                                    { $lte: [ "$$num", 3 ] }
                                ] }}}, "$int_arr"],
             [["a", "b", "c", 2, 3, 1, 2, 3, 4]]);

// Confirm that empty arrays can be concatenated with variables.
runAndAssert(
    ["$str_arr", {$filter: {input: [], cond: {$isArray: [{$concatArrays: [[], "$$this"]}]}}}],
    [["a", "b", "c"]]);

// Concatenation with no arguments results in the empty array.
runAndAssert([], [[]]);

// Confirm that having any combination of null or missing inputs and valid inputs produces null.
runAndAssertNull(["$int_arr", "$null_val"]);
runAndAssertNull(["$int_arr", null]);
runAndAssertNull([null, "$int_arr", "$str_arr"]);
runAndAssertNull(["$int_arr", null, "$str_arr"]);
runAndAssertNull(["$null_val", "$str_arr", "$int_arr"]);
runAndAssertNull(["$str_arr", "$null_val", "$int_arr"]);
runAndAssertNull(["$int_arr", "$not_a_field"]);
runAndAssertNull(["$not_a_field", "$str_arr", "$int_arr"]);
runAndAssertNull(["$not_a_field"]);
runAndAssertNull(["$null_val"]);
runAndAssertNull(["$not_a_field", "$null_val"]);
runAndAssertNull(["$null_val", "$not_a_field"]);
runAndAssertNull([
    {$concatArrays: "$int_arr"},
    null,
    {$concatArrays: {$concatArrays: ["$obj_arr", "$str_arr"]}}
]);

// Confirm edge case where if null precedes non-array input, null is returned.
runAndAssertNull(["$int_arr", "$null_val", "$int_val"]);
runAndAssertNull(["$null_val", null, "$null_val"]);

//
// Confirm error cases.
//

// Confirm concatenating non-array and non-values produces an error.
runAndAssertThrows(["$dbl_val"]);
runAndAssertThrows(["$str_val"]);
runAndAssertThrows(["$int_val"]);
runAndAssertThrows([123]);
runAndAssertThrows(["some_val", [1, 2, 3]]);
runAndAssertThrows(["$obj_val"]);
runAndAssertThrows(["$int_arr", "$int_val"]);
runAndAssertThrows(["$dbl_arr", "$dbl_val"]);

// Confirm edge case where if invalid input precedes null or missing inputs, the command fails.
// Note that when the SBE engine is enabled, null will be returned before invalid input because
// we check if any values are null before checking whether all values are arrays.
let evalFn = checkSBEEnabled(db) ? runAndAssertNull : runAndAssertThrows;
evalFn(["$int_arr", "$dbl_val", "$null_val"]);
evalFn(["$int_arr", "some_string_value", "$null_val"]);
evalFn(["$dbl_val", "$null_val"]);
evalFn(["$int_arr", "$int_val", "$not_a_field"]);
evalFn(["$int_val", "$not_a_field"]);
evalFn(["$int_val", "$not_a_field", "$null_val"]);
runAndAssertThrows(["$int_arr", 32]);

// Clear collection.
assert(coll.drop());

// Test case where find returns multiple documents.
assert.commandWorked(coll.insertMany([
    {arr1: [42, 35.0, 197865432], arr2: ["albatross", "abbacus", "alien"]},
    {arr1: [1], arr2: ["albatross", "abbacus", "alien"]},
    {arr1: [1, 2, 3, 4, 5, 6, 11, 12, 23], arr2: []},
    {arr1: [], arr2: ["foo", "bar"]},
    {arr1: [], arr2: []},
    {arr1: [1, 2, 3, 4, 5, 6, 11, 12, 23], arr2: null},
    {some_field: "foo"},
]));
runAndAssert(["$arr1", "$arr2"], [
    [42, 35.0, 197865432, "albatross", "abbacus", "alien"],
    [1, "albatross", "abbacus", "alien"],
    [1, 2, 3, 4, 5, 6, 11, 12, 23],
    ["foo", "bar"],
    [],
    null,
    null
]);
runAndAssert(["$arr1", [1, 2, 3], "$arr2"], [
    [42, 35.0, 197865432, 1, 2, 3, "albatross", "abbacus", "alien"],
    [1, 1, 2, 3, "albatross", "abbacus", "alien"],
    [1, 2, 3, 4, 5, 6, 11, 12, 23, 1, 2, 3],
    ["foo", 1, 2, 3, "bar"],
    [1, 2, 3],
    null,
    null
]);
}());
