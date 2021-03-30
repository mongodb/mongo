/**
 * Tests basic functionality of the $getField expression.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.

const coll = db.expression_get_field;
coll.drop();

for (let i = 0; i < 2; i++) {
    assert.commandWorked(coll.insert({
        _id: i,
        x: i,
        y: "c",
        "a$b": "foo",
        "a.b": "bar",
        "a.$b": 5,
        ".xy": i,
        ".$xz": i,
        "..zz": i,
        c: {d: "x"},
    }));
}

// Test that $getField fails with the provided 'code' for invalid arguments 'getFieldArgs'.
function assertGetFieldFailedWithCode(getFieldArgs, code) {
    const error =
        assert.throws(() => coll.aggregate([{$project: {test: {$getField: getFieldArgs}}}]));
    assert.commandFailedWithCode(error, code);
}

// Test that $getField returns the 'expected' results for the given arguments 'getFieldArgs'.
function assertGetFieldResultsEq(getFieldArgs, expected) {
    assertPipelineResultsEq([{$project: {_id: 1, test: {$getField: getFieldArgs}}}], expected);
}

// Test the given 'pipeline' returns the 'expected' results.
function assertPipelineResultsEq(pipeline, expected) {
    const actual = coll.aggregate(pipeline).toArray();
    assertArrayEq({actual, expected});
}

const isDotsAndDollarsEnabled = db.adminCommand({getParameter: 1, featureFlagDotsAndDollars: 1})
                                    .featureFlagDotsAndDollars.value;

if (!isDotsAndDollarsEnabled) {
    // Verify that $getField is not available if the feature flag is set to false and don't
    // run the rest of the test.
    assertGetFieldFailedWithCode({field: "a", from: {a: "b"}}, 31325);
    return;
}

// Test that $getField fails with a document missing named arguments.
assertGetFieldFailedWithCode({from: {a: "b"}}, 3041702);
assertGetFieldFailedWithCode({field: "a"}, 3041703);

// Test that $getField fails with a document with one or more arguments of incorrect type.
assertGetFieldFailedWithCode({field: true, from: {a: "b"}}, 3041704);
assertGetFieldFailedWithCode({field: {"a": 1}, from: {"a": 1}}, 3041704);
assertGetFieldFailedWithCode({field: "a", from: true}, 3041705);
assertGetFieldFailedWithCode(5, 3041704);
assertGetFieldFailedWithCode(true, 3041704);
assertGetFieldFailedWithCode({$add: [2, 3]}, 3041704);

// Test that $getField fails with a document with invalid arguments.
assertGetFieldFailedWithCode({field: "a", from: {a: "b"}, unknown: true}, 3041701);

// Test that $getField returns the correct value from the provided object.
assertGetFieldResultsEq({field: "a", from: {a: "b"}}, [{_id: 0, test: "b"}, {_id: 1, test: "b"}]);

// Test that $getField returns the correct value from the $$ROOT object.
assertGetFieldResultsEq(null, [{_id: 0, test: null}, {_id: 1, test: null}]);
assertGetFieldResultsEq("a", [{_id: 0}, {_id: 1}]);  // The test field should evaluate to missing.
assertGetFieldResultsEq("a$b", [{_id: 0, test: "foo"}, {_id: 1, test: "foo"}]);
assertGetFieldResultsEq("a.b", [{_id: 0, test: "bar"}, {_id: 1, test: "bar"}]);
assertGetFieldResultsEq("x", [{_id: 0, test: 0}, {_id: 1, test: 1}]);
assertGetFieldResultsEq("a.$b", [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq(".xy", [{_id: 0, test: 0}, {_id: 1, test: 1}]);
assertGetFieldResultsEq(".$xz", [{_id: 0, test: 0}, {_id: 1, test: 1}]);
assertGetFieldResultsEq("..zz", [{_id: 0, test: 0}, {_id: 1, test: 1}]);

// Test that $getField returns the correct value from the $$ROOT object when field is an expression.
assertGetFieldResultsEq({$concat: ["a", "b"]},
                        [{_id: 0}, {_id: 1}]);  // The test field should evaluate to missing.
assertGetFieldResultsEq({$concat: ["a", {$const: "$"}, "b"]},
                        [{_id: 0, test: "foo"}, {_id: 1, test: "foo"}]);
assertGetFieldResultsEq({$cond: [true, null, "x"]}, [{_id: 0, test: null}, {_id: 1, test: null}]);
assertGetFieldResultsEq({$cond: [false, null, "x"]}, [{_id: 0, test: 0}, {_id: 1, test: 1}]);

// Test that $getField treats dotted fields as key literals instead of field paths. Note that it is
// necessary to use $const in places, otherwise object field validation would reject some of these
// field names.
assertGetFieldResultsEq({field: "a.b", from: {$const: {"a.b": "b"}}},
                        [{_id: 0, test: "b"}, {_id: 1, test: "b"}]);
assertGetFieldResultsEq({field: ".ab", from: {$const: {".ab": "b"}}},
                        [{_id: 0, test: "b"}, {_id: 1, test: "b"}]);
assertGetFieldResultsEq({field: "ab.", from: {$const: {"ab.": "b"}}},
                        [{_id: 0, test: "b"}, {_id: 1, test: "b"}]);
assertGetFieldResultsEq({field: "a.b.c", from: {$const: {"a.b.c": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq({field: "a.b.c", from: {a: {b: {c: 5}}}},
                        [{_id: 0}, {_id: 1}]);  // The test field should evaluate to missing.
assertGetFieldResultsEq({field: {$concat: ["a.b", ".", "c"]}, from: {$const: {"a.b.c": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq({field: {$concat: ["a.b", ".", "c"]}, from: {a: {b: {c: 5}}}},
                        [{_id: 0}, {_id: 1}]);  // The test field should evaluate to missing.

// Test that $getField works with fields that contain '$'.
assertGetFieldResultsEq({field: "a$b", from: {"a$b": "b"}},
                        [{_id: 0, test: "b"}, {_id: 1, test: "b"}]);
assertGetFieldResultsEq({field: "a$b.b", from: {$const: {"a$b.b": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq({field: {$const: "a$b.b"}, from: {$const: {"a$b.b": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq({field: {$const: "$b.b"}, from: {$const: {"$b.b": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq({field: {$const: "$b"}, from: {$const: {"$b": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq({field: {$const: "$.ab"}, from: {$const: {"$.ab": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq({field: {$const: "$$xz"}, from: {$const: {"$$xz": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);

// Test null and missing cases.
assertGetFieldResultsEq({field: "a", from: null}, [{_id: 0, test: null}, {_id: 1, test: null}]);
assertGetFieldResultsEq({field: null, from: {a: 1}}, [{_id: 0, test: null}, {_id: 1, test: null}]);
assertGetFieldResultsEq({field: "a", from: {b: 2, c: 3}},
                        [{_id: 0}, {_id: 1}]);  // The test field should evaluate to missing.
assertGetFieldResultsEq({field: "a", from: {a: null, b: 2, c: 3}},
                        [{_id: 0, test: null}, {_id: 1, test: null}]);
assertGetFieldResultsEq({field: {$const: "$a"}, from: {$const: {"$a": null, b: 2, c: 3}}},
                        [{_id: 0, test: null}, {_id: 1, test: null}]);
assertGetFieldResultsEq({field: "a", from: {}},
                        [{_id: 0}, {_id: 1}]);  // The test field should evaluate to missing.

// These should return null because "$a.b" evaluates to a field path expression which returns a
// nullish value (so the expression should return null), as there is no $a.b field path.
assertGetFieldResultsEq({field: "$a.b", from: {$const: {"$a.b": 5}}},
                        [{_id: 0, test: null}, {_id: 1, test: null}]);
assertGetFieldResultsEq("$a.b", [{_id: 0, test: null}, {_id: 1, test: null}]);

// When the field path does actually resolve to a field, the value of that field should be used.

// The fieldpath $y resolves to "c" in $$ROOT.
assertGetFieldResultsEq({field: "$y", from: {$const: {"c": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq({field: "$y", from: {$const: {"a": 5}}},
                        [{_id: 0}, {_id: 1}]);  // The test field should evaluate to missing.
assertGetFieldResultsEq("$y", [{_id: 0, test: {d: "x"}}, {_id: 1, test: {d: "x"}}]);

// The fieldpath $c.d resolves to "x" in $$ROOT.
assertGetFieldResultsEq({field: "$c.d", from: {$const: {"x": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq({field: "$c.d", from: {$const: {"y": 5}}},
                        [{_id: 0}, {_id: 1}]);  // The test field should evaluate to missing.
assertGetFieldResultsEq("$c.d", [{_id: 0, test: 0}, {_id: 1, test: 1}]);

// $x resolves to a number, so this should fail.
assertGetFieldFailedWithCode({field: "$x", from: {$const: {"c": 5}}}, 3041704);
assertGetFieldFailedWithCode("$x", 3041704);

// Test case where $getField stages are nested.
assertGetFieldResultsEq(
    {field: "a", from: {$getField: {field: "b.c", from: {$const: {"b.c": {a: 5}}}}}},
    [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq(
    {field: "x", from: {$getField: {field: "b.c", from: {$const: {"b.c": {a: 5}}}}}},
    [{_id: 0}, {_id: 1}]);
assertGetFieldResultsEq(
    {field: "a", from: {$getField: {field: "b.d", from: {$const: {"b.c": {a: 5}}}}}},
    [{_id: 0, test: null}, {_id: 1, test: null}]);

// Test case when a dotted/dollar path is within an array.
assertGetFieldResultsEq({
    field: {$const: "a$b"},
    from: {$arrayElemAt: [[{$const: {"a$b": 1}}, {$const: {"a$b": 2}}], 0]}
},
                        [{_id: 0, test: 1}, {_id: 1, test: 1}]);
assertGetFieldResultsEq({
    field: {$const: "a.."},
    from: {$arrayElemAt: [[{$const: {"a..": 1}}, {$const: {"a..": 2}}], 1]}
},
                        [{_id: 0, test: 2}, {_id: 1, test: 2}]);

// Test $getField expression with other pipeline stages.

assertPipelineResultsEq(
    [
        {$match: {$expr: {$eq: [{$getField: "_id"}, {$getField: ".$xz"}]}}},
        {$project: {aa: {$getField: ".$xz"}, "_id": 1}},
    ],
    [{_id: 0, aa: 0}, {_id: 1, aa: 1}]);

assertPipelineResultsEq([{$match: {$expr: {$ne: [{$getField: "_id"}, {$getField: ".$xz"}]}}}], []);
assertPipelineResultsEq(
    [
        {$match: {$expr: {$ne: [{$getField: "_id"}, {$getField: "a.b"}]}}},
        {$project: {"a": {$getField: "x"}, "b": {$getField: {$const: "a.b"}}}}
    ],
    [{_id: 0, a: 0, b: "bar"}, {_id: 1, a: 1, b: "bar"}]);

assertPipelineResultsEq(
    [
        {$addFields: {aa: {$getField: {$const: "a.b"}}}},
        {$project: {aa: 1, _id: 1}},
    ],
    [{_id: 0, aa: "bar"}, {_id: 1, aa: "bar"}]);

assertPipelineResultsEq(
    [
        {$bucket: {groupBy: {$getField: {$const: "a.b"}}, boundaries: ["aaa", "bar", "zzz"]}}
    ],  // We should get one bucket here ("bar") with two documents.
    [{_id: "bar", count: 2}]);
assertPipelineResultsEq([{
                            $bucket: {groupBy: {$getField: "x"}, boundaries: [0, 1, 2, 3, 4]}
                        }],  // We should get two buckets here for the two possible values of x.
                        [{_id: 0, count: 1}, {_id: 1, count: 1}]);
})();