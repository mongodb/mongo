// SERVER-29425 added a new expression, $sortArray, which consumes an array or a nullish value
// and produces either the sorted version of that array, or null. In this test file, we check the
// behavior and error cases.
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.
load("jstests/aggregation/extras/utils.js");        // For assertErrorCode.

(function() {
"use strict";

let coll = db.sortArray;
coll.drop();

assert.commandWorked(coll.insert({
    nullField: null,
    undefField: undefined,
    embedded: [[1, 2], [3, 4]],
    singleElem: [1],
    normal: [1, 2, 3],
    num: 1,
    empty: [],
    normalSingleObjs: [{a: 1}, {a: 2}, {a: 3}],
    mismatchedSingleObjs: [{a: 1}, {b: 2}, {c: 3}],
    normalMultiObjs: [{a: 1, b: 3, c: 1}, {a: 2, b: 2, c: 2}, {a: 3, b: 1, c: 3}],
    tiesMultiObjs: [{a: 1, b: 2, c: 1}, {a: 1, b: 3, c: 4}, {a: 1, b: 3, c: 5}],
    nestedObjs: [{a: 1, b: {c: 2}}, {a: 1, b: {c: 1}}],
    mismatchedTypes: [1, [1], {a: 1}, "1"],
    moreMismatchedTypes: [2, 1, "hello", {a: 6}, {a: "hello"}, {a: -1}, null],
    mismatchedNumberTypes: [[NumberDecimal(4)], [1, 9, 8]],
    collatorTestField: ["2", "10"],
    collatorObjectTestField: [{a: "2"}, {a: "10"}],

}));

let assertDBOutputEquals = (expected, output) => {
    output = output.toArray();
    assert.eq(1, output.length);
    assert.eq(expected, output[0].sorted);
};

assertErrorCode(coll, [{$project: {sorted: {$sortArray: 1}}}], 2942500);
assertErrorCode(coll, [{$project: {sorted: {$sortArray: "$num"}}}], 2942500);

assertDBOutputEquals([1, 2, 3], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: {$literal: [1, 2, 3]}, sortBy: 1}}}}
]));

assertDBOutputEquals(
    [3, 2, 1],
    coll.aggregate([{$project: {sorted: {$sortArray: {input: "$normal", sortBy: -1}}}}]));

assertDBOutputEquals([1, 2, 3], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: {$literal: [3, 2, 1]}, sortBy: 1}}}}
]));

assertDBOutputEquals([3, 2, 1], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: {$literal: [3, 2, 1]}, sortBy: -1}}}}
]));

assertDBOutputEquals(
    null, coll.aggregate([{$project: {sorted: {$sortArray: {input: "$notAField", sortBy: 1}}}}]));

assertDBOutputEquals([[1, 2], [3, 4]], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: {$literal: [[1, 2], [3, 4]]}, sortBy: 1}}}}
]));

assertDBOutputEquals(
    [[3, 4], [1, 2]],
    coll.aggregate([{$project: {sorted: {$sortArray: {input: "$embedded", sortBy: -1}}}}]));

assertDBOutputEquals(
    null,
    coll.aggregate([{$project: {sorted: {$sortArray: {input: {$literal: null}, sortBy: 1}}}}]));

assertDBOutputEquals(
    null, coll.aggregate([{$project: {sorted: {$sortArray: {input: "$nullField", sortBy: -1}}}}]));

assertDBOutputEquals(null, coll.aggregate([
    {$project: {sorted: {$sortArray: {input: {$literal: undefined}, sortBy: 1}}}}
]));

assertDBOutputEquals(
    null, coll.aggregate([{$project: {sorted: {$sortArray: {input: "$undefField", sortBy: -1}}}}]));

assertDBOutputEquals(
    [1], coll.aggregate([{$project: {sorted: {$sortArray: {input: {$literal: [1]}, sortBy: 1}}}}]));

assertDBOutputEquals(
    [1], coll.aggregate([{$project: {sorted: {$sortArray: {input: "$singleElem", sortBy: -1}}}}]));

assertDBOutputEquals(
    [], coll.aggregate([{$project: {sorted: {$sortArray: {input: {$literal: []}, sortBy: 1}}}}]));

assertDBOutputEquals(
    [], coll.aggregate([{$project: {sorted: {$sortArray: {input: "$empty", sortBy: -1}}}}]));

/* ------------------------ Object Array Tests ------------------------ */

// Test that we handle the case of satisfying "Compare" requirements with -1 sort (SERVER-61941).
assertDBOutputEquals([[], [], {}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: {$literal: [{}, [], []]}, sortBy: -1}}}}
]));

assertDBOutputEquals([{}, {}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: {$literal: [{}, {}]}, sortBy: -1}}}}
]));

assertDBOutputEquals([{a: 1}, {a: 2}, {a: 3}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$normalSingleObjs", sortBy: {a: 1}}}}}
]));

assertDBOutputEquals([{a: 3}, {a: 2}, {a: 1}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$normalSingleObjs", sortBy: {a: -1}}}}}
]));

assertDBOutputEquals([{a: 1}, {a: 2}, {a: 3}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$normalSingleObjs", sortBy: {b: 1}}}}}
]));

assertDBOutputEquals([{a: 1}, {a: 2}, {a: 3}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$normalSingleObjs", sortBy: {b: -1}}}}}
]));

assertDBOutputEquals(
    [{a: 1}, {a: 2}, {a: 3}],
    coll.aggregate([{$project: {sorted: {$sortArray: {input: "$normalSingleObjs", sortBy: 1}}}}]));

assertDBOutputEquals(
    [{a: 3}, {a: 2}, {a: 1}],
    coll.aggregate([{$project: {sorted: {$sortArray: {input: "$normalSingleObjs", sortBy: -1}}}}]));

assertDBOutputEquals([{a: 1}, {b: 2}, {c: 3}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$mismatchedSingleObjs", sortBy: 1}}}}
]));

assertDBOutputEquals([{c: 3}, {b: 2}, {a: 1}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$mismatchedSingleObjs", sortBy: -1}}}}
]));

assertDBOutputEquals([{b: 2}, {c: 3}, {a: 1}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$mismatchedSingleObjs", sortBy: {a: 1}}}}}
]));

assertDBOutputEquals([{a: 1}, {c: 3}, {b: 2}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$mismatchedSingleObjs", sortBy: {b: 1}}}}}
]));

assertDBOutputEquals([{a: 1}, {b: 2}, {c: 3}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$mismatchedSingleObjs", sortBy: {c: 1}}}}}
]));

assertDBOutputEquals([{a: 3, b: 1, c: 3}, {a: 2, b: 2, c: 2}, {a: 1, b: 3, c: 1}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$normalMultiObjs", sortBy: {b: 1, a: 1}}}}}
]));

assertDBOutputEquals([{a: 1, b: 3, c: 1}, {a: 2, b: 2, c: 2}, {a: 3, b: 1, c: 3}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$normalMultiObjs", sortBy: {b: -1, a: 1}}}}}
]));

assertDBOutputEquals([{a: 1, b: 2, c: 1}, {a: 1, b: 3, c: 4}, {a: 1, b: 3, c: 5}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$tiesMultiObjs", sortBy: {a: 1, b: 1, c: 1}}}}}
]));

assertDBOutputEquals([{a: 1, b: 2, c: 1}, {a: 1, b: 3, c: 5}, {a: 1, b: 3, c: 4}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$tiesMultiObjs", sortBy: {a: 1, b: 1, c: -1}}}}}
]));

/* ------------------------ Nested Objects Tests ------------------------ */

assertDBOutputEquals([{a: 1, b: {c: 1}}, {a: 1, b: {c: 2}}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$nestedObjs", sortBy: {"b.c": 1}}}}}
]));

assertDBOutputEquals([{a: 1, b: {c: 2}}, {a: 1, b: {c: 1}}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$nestedObjs", sortBy: {"b.c": -1}}}}}
]));

/* ------------------------ Mismatched Types Tests ------------------------ */

assertDBOutputEquals(
    [1, "1", {a: 1}, [1]],
    coll.aggregate([{$project: {sorted: {$sortArray: {input: "$mismatchedTypes", sortBy: 1}}}}]));

assertDBOutputEquals([null, 1, 2, "hello", {a: -1}, {a: 6}, {a: "hello"}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$moreMismatchedTypes", sortBy: 1}}}}
]));

assertDBOutputEquals([[1, 9, 8], [NumberDecimal(4)]], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$mismatchedNumberTypes", sortBy: 1}}}}
]));

/* ------------------------ Collator Tests ------------------------ */

assertDBOutputEquals(
    ["10", "2"],
    coll.aggregate([{$project: {sorted: {$sortArray: {input: "$collatorTestField", sortBy: 1}}}}]));

assertDBOutputEquals(
    ["2", "10"],
    coll.aggregate([{$project: {sorted: {$sortArray: {input: "$collatorTestField", sortBy: 1}}}}],
                   {collation: {locale: "en", numericOrdering: true}}));

assertDBOutputEquals([{a: "10"}, {a: "2"}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$collatorObjectTestField", sortBy: 1}}}}
]));

assertDBOutputEquals(
    [{a: "2"}, {a: "10"}],
    coll.aggregate(
        [{$project: {sorted: {$sortArray: {input: "$collatorObjectTestField", sortBy: 1}}}}],
        {collation: {locale: "en", numericOrdering: true}}));

assertDBOutputEquals([{a: "10"}, {a: "2"}], coll.aggregate([
    {$project: {sorted: {$sortArray: {input: "$collatorObjectTestField", sortBy: {a: 1}}}}}
]));

assertDBOutputEquals(
    [{a: "2"}, {a: "10"}],
    coll.aggregate(
        [{$project: {sorted: {$sortArray: {input: "$collatorObjectTestField", sortBy: {a: 1}}}}}],
        {collation: {locale: "en", numericOrdering: true}}));
}());
