/**
 * Tests basic functionality of the $getField expression.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.

const coll = db.expression_get_field;
coll.drop();

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
        "$a": 10,
        "$x.$y": 20,
        "$x..$y": {"$a": 1, "$b..$c": 2},
        c: {d: "x"},
        e: {"$f": 30},
        f: [{"$a": 41}, {"$b..": 42}],
        "$v..": null
    }));
}

// Test that $getField fails with a document missing named arguments.
assertGetFieldFailedWithCode({input: {a: "b"}}, 3041702);
assertGetFieldFailedWithCode({field: "a"}, 3041703);

// Test that $getField fails with a document with one or more arguments of incorrect type.
assertGetFieldFailedWithCode({field: true, input: {a: "b"}}, 5654602);
assertGetFieldFailedWithCode({field: {"a": 1}, input: {"a": 1}}, 5654601);
assertGetFieldFailedWithCode(5, 5654602);
assertGetFieldFailedWithCode(true, 5654602);
assertGetFieldFailedWithCode({field: null, input: {"a": 1}}, 5654602);

// Test that $getField fails with a document with invalid arguments.
assertGetFieldFailedWithCode({field: "a", input: {a: "b"}, unknown: true}, 3041701);

// Test that $getField fails when 'field' argument is a field reference.
assertGetFieldFailedWithCode({field: "$a", input: {a: "b"}}, 5654600);
assertGetFieldFailedWithCode({field: "$a.b", input: {a: "b"}}, 5654600);
assertGetFieldFailedWithCode({field: "$$CURRENT.a", input: {a: "b"}}, 5654600);

// Test that $getField fails when 'field' argument is an arbitrary expression other than '$const'
// String.
assertGetFieldFailedWithCode({$add: [2, 3]}, 5654601);
assertGetFieldFailedWithCode({field: {$concat: ["a", "b"]}, input: {"a": 1}}, 5654601);
assertGetFieldFailedWithCode({field: {$cond: [false, null, "x"]}, input: {"a": 1}}, 5654601);
assertGetFieldFailedWithCode({$const: true}, 5654602);
assertGetFieldFailedWithCode({$const: {"a": 1}}, 5654602);
assertGetFieldFailedWithCode({field: {$const: []}, input: {"a": 1}}, 5654602);

// Test that $getField returns the correct value from the provided object.
assertGetFieldResultsEq({field: "a", input: {a: "b"}}, [{_id: 0, test: "b"}, {_id: 1, test: "b"}]);

// Test that $getField returns the correct value from the $$CURRENT object.
assertGetFieldResultsEq("a", [{_id: 0}, {_id: 1}]);  // The test field should evaluate to missing.
assertGetFieldResultsEq("a$b", [{_id: 0, test: "foo"}, {_id: 1, test: "foo"}]);
assertGetFieldResultsEq("a.b", [{_id: 0, test: "bar"}, {_id: 1, test: "bar"}]);
assertGetFieldResultsEq("x", [{_id: 0, test: 0}, {_id: 1, test: 1}]);
assertGetFieldResultsEq("a.$b", [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq(".xy", [{_id: 0, test: 0}, {_id: 1, test: 1}]);
assertGetFieldResultsEq(".$xz", [{_id: 0, test: 0}, {_id: 1, test: 1}]);
assertGetFieldResultsEq("..zz", [{_id: 0, test: 0}, {_id: 1, test: 1}]);
assertGetFieldResultsEq({$const: "$a"}, [{_id: 0, test: 10}, {_id: 1, test: 10}]);
assertGetFieldResultsEq({$const: "$x.$y"}, [{_id: 0, test: 20}, {_id: 1, test: 20}]);
assertGetFieldResultsEq(
    {$const: "$x..$y"},
    [{_id: 0, test: {"$a": 1, "$b..$c": 2}}, {_id: 1, test: {"$a": 1, "$b..$c": 2}}]);
assertGetFieldResultsEq({field: {$const: "$f"}, input: "$e"},
                        [{_id: 0, test: 30}, {_id: 1, test: 30}]);

// Test that $getField treats dotted fields as key literals instead of field paths. Note that it is
// necessary to use $const in places, otherwise object field validation would reject some of these
// field names.
assertGetFieldResultsEq({field: "a.b", input: {$const: {"a.b": "b"}}},
                        [{_id: 0, test: "b"}, {_id: 1, test: "b"}]);
assertGetFieldResultsEq({field: ".ab", input: {$const: {".ab": "b"}}},
                        [{_id: 0, test: "b"}, {_id: 1, test: "b"}]);
assertGetFieldResultsEq({field: "ab.", input: {$const: {"ab.": "b"}}},
                        [{_id: 0, test: "b"}, {_id: 1, test: "b"}]);
assertGetFieldResultsEq({field: "a.b.c", input: {$const: {"a.b.c": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq({field: "a.b.c", input: {a: {b: {c: 5}}}},
                        [{_id: 0}, {_id: 1}]);  // The test field should evaluate to missing.
assertGetFieldResultsEq({field: "d", input: {$getField: "c"}},
                        [{_id: 0, "test": "x"}, {_id: 1, "test": "x"}]);

// Test that $getField works with fields that contain '$'.
assertGetFieldResultsEq({field: "a$b", input: {"a$b": "b"}},
                        [{_id: 0, test: "b"}, {_id: 1, test: "b"}]);
assertGetFieldResultsEq({field: "a$b.b", input: {$const: {"a$b.b": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq({field: {$const: "a$b.b"}, input: {$const: {"a$b.b": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq({field: {$const: "$b.b"}, input: {$const: {"$b.b": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq({field: {$const: "$b"}, input: {$const: {"$b": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq({field: {$const: "$.ab"}, input: {$const: {"$.ab": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq({field: {$const: "$$xz"}, input: {$const: {"$$xz": 5}}},
                        [{_id: 0, test: 5}, {_id: 1, test: 5}]);

// Test null and missing cases.
assertGetFieldResultsEq({field: "a", input: null}, [{_id: 0, test: null}, {_id: 1, test: null}]);
assertGetFieldResultsEq({field: "a", input: {b: 2, c: 3}},
                        [{_id: 0}, {_id: 1}]);  // The test field should evaluate to missing.
assertGetFieldResultsEq({field: "a", input: {a: null, b: 2, c: 3}},
                        [{_id: 0, test: null}, {_id: 1, test: null}]);
assertGetFieldResultsEq({field: {$const: "$a"}, input: {$const: {"$a": null, b: 2, c: 3}}},
                        [{_id: 0, test: null}, {_id: 1, test: null}]);
assertGetFieldResultsEq({field: "a", input: {}},
                        [{_id: 0}, {_id: 1}]);  // The test field should evaluate to missing.
assertGetFieldResultsEq({$const: "$v.."}, [{_id: 0, test: null}, {_id: 1, test: null}]);
assertGetFieldResultsEq({$const: "$u.."},
                        [{_id: 0}, {_id: 1}]);  // The test field should evaluate to missing.
assertGetFieldResultsEq({field: "doesNotExist2", input: {$getField: "doesNotExist1"}},
                        [{_id: 0}, {_id: 1}]);
assertGetFieldResultsEq({field: "x", input: {$getField: "doesNotExist"}}, [{_id: 0}, {_id: 1}]);
assertGetFieldResultsEq({field: "a", input: true}, [{_id: 0}, {_id: 1}]);

// Test case where $getField stages are nested.
assertGetFieldResultsEq(
    {field: "a", input: {$getField: {field: "b.c", input: {$const: {"b.c": {a: 5}}}}}},
    [{_id: 0, test: 5}, {_id: 1, test: 5}]);
assertGetFieldResultsEq(
    {field: "x", input: {$getField: {field: "b.c", input: {$const: {"b.c": {a: 5}}}}}},
    [{_id: 0}, {_id: 1}]);
assertGetFieldResultsEq(
    {field: "a", input: {$getField: {field: "b.d", input: {$const: {"b.c": {a: 5}}}}}},
    [{_id: 0}, {_id: 1}]);
assertGetFieldResultsEq({field: {$const: "$a"}, input: {$getField: {$const: "$x..$y"}}},
                        [{_id: 0, test: 1}, {_id: 1, test: 1}]);
assertGetFieldResultsEq({field: {$const: "$b..$c"}, input: {$getField: {$const: "$x..$y"}}},
                        [{_id: 0, test: 2}, {_id: 1, test: 2}]);

// Test case when a dotted/dollar path is within an array.
assertGetFieldResultsEq({
    field: {$const: "a$b"},
    input: {$arrayElemAt: [[{$const: {"a$b": 1}}, {$const: {"a$b": 2}}], 0]}
},
                        [{_id: 0, test: 1}, {_id: 1, test: 1}]);
assertGetFieldResultsEq({
    field: {$const: "a.."},
    input: {$arrayElemAt: [[{$const: {"a..": 1}}, {$const: {"a..": 2}}], 1]}
},
                        [{_id: 0, test: 2}, {_id: 1, test: 2}]);
assertGetFieldResultsEq({field: {$const: "$a"}, input: {$arrayElemAt: ["$f", 0]}},
                        [{_id: 0, test: 41}, {_id: 1, test: 41}]);
assertGetFieldResultsEq({field: {$const: "$b.."}, input: {$arrayElemAt: ["$f", 1]}},
                        [{_id: 0, test: 42}, {_id: 1, test: 42}]);

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
