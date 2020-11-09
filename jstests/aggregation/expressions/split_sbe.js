// Tests for the $split expression, for the SBE and classic engines. This file was created for
// SERVER-51554 because a couple tests in jstests/aggregation/expressions/split.js didn't work in
// SBE due to bugs in value::TypeTags::StringSmall and value::TypeTags::StringBig (SERVER-52818).
// When resolved, this file can be combined with split.js.

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode and testExpression.
load("jstests/libs/sbe_assert_error_override.js");

const coll = db.split;

coll.drop();
assert.commandWorked(coll.insert({}));

//
// Tests with constant-folding optimization.
//

// Basic tests.
testExpression(coll, {$split: ["abc", "b"]}, ["a", "c"]);
testExpression(coll, {$split: ["aaa", "b"]}, ["aaa"]);
testExpression(coll, {$split: ["a b a", "b"]}, ["a ", " a"]);
testExpression(coll, {$split: ["a", "a"]}, ["", ""]);
testExpression(coll, {$split: ["aa", "a"]}, ["", "", ""]);
testExpression(coll, {$split: ["aaa", "a"]}, ["", "", "", ""]);
testExpression(coll, {$split: ["", "a"]}, [""]);
testExpression(coll, {$split: ["abc abc cba abc", "abc"]}, ["", " ", " cba ", ""]);

// Ensure that $split operates correctly when the string has multi-byte tokens or input strings.
testExpression(coll, {$split: ["∫a∫", "a"]}, ["∫", "∫"]);
testExpression(coll, {$split: ["a∫∫a", "∫"]}, ["a", "", "a"]);

// Ensure that $split produces null when given null as input.
testExpression(coll, {$split: ["abc", null]}, null);
testExpression(coll, {$split: [null, "abc"]}, null);

// Ensure that $split produces null when given missing fields as input.
testExpression(coll, {$split: ["$a", "a"]}, null);
testExpression(coll, {$split: ["a", "$a"]}, null);

//
// Tests without constant-folding optimization.
//

// Basic tests.
assert(coll.drop());
assert.commandWorked(coll.insert([
    {string: "abc"},
    {string: "a"},
    {string: "aa"},
    {string: "aaa"},
    {string: "bbb"},
    {string: "b a b"},
    {string: ""},
    {string: "a a b a"},
]));

let res = coll.aggregate({$project: {_id: 0, output: {$split: ["$string", "a"]}}}).toArray();
assert(arrayEq(res, [
    {output: ["", "bc"]},
    {output: ["", ""]},
    {output: ["", "", ""]},
    {output: ["", "", "", ""]},
    {output: ["bbb"]},
    {output: ["b ", " b"]},
    {output: [""]},
    {output: ["", " ", " b ", ""]},
]));

// Ensure that $split operates correctly when the string has multi-byte tokens or input strings.
assert(coll.drop());
assert.commandWorked(coll.insert([
    {string: "∫a∫"},
    {string: "a∫∫a"},
]));

res = coll.aggregate({$project: {_id: 0, output: {$split: ["$string", "a"]}}}).toArray();

assert(arrayEq(res, [
    {output: ["∫", "∫"]},
    {output: ["", "∫∫", ""]},
]));

res = coll.aggregate({$project: {_id: 0, output: {$split: ["$string", "∫"]}}}).toArray();

assert(arrayEq(res, [
    {output: ["", "a", ""]},
    {output: ["a", "", "a"]},
]));

// Ensure that $split produces null when given null as input.
assert(coll.drop());
assert.commandWorked(coll.insert({string: "abc"}));
res = coll.aggregate({$project: {_id: 0, output: {$split: ["$string", null]}}}).toArray();
assert(arrayEq(res, [{output: null}]));

assert(coll.drop());
assert.commandWorked(coll.insert({string: null}));
res = coll.aggregate({$project: {_id: 0, output: {$split: ["$string", "abc"]}}}).toArray();
assert(arrayEq(res, [{output: null}]));

// Ensure that $split produces null when given missing fields as input.
res = coll.aggregate({$project: {_id: 0, output: {$split: ["$a", "a"]}}}).toArray();
assert(arrayEq(res, [{output: null}]));
res = coll.aggregate({$project: {_id: 0, output: {$split: ["a", "$a"]}}}).toArray();
assert(arrayEq(res, [{output: null}]));

//
// Error Code tests with constant-folding optimization.
//

// Ensure that $split errors when given more or less than two arguments.
var pipeline = {$project: {split: {$split: []}}};
assertErrorCode(coll, pipeline, 16020);

pipeline = {
    $project: {split: {$split: ["a"]}}
};
assertErrorCode(coll, pipeline, 16020);

pipeline = {
    $project: {split: {$split: ["a", "b", "c"]}}
};
assertErrorCode(coll, pipeline, 16020);

// Ensure that $split errors when given non-string input.
pipeline = {
    $project: {split: {$split: [1, "abc"]}}
};
assertErrorCode(coll, pipeline, 40085);

pipeline = {
    $project: {split: {$split: ["abc", 1]}}
};
assertErrorCode(coll, pipeline, 40086);

// Ensure that $split errors when given an empty separator.
pipeline = {
    $project: {split: {$split: ["abc", ""]}}
};
assertErrorCode(coll, pipeline, 40087);

//
// Error Code tests without constant-folding optimization.
//

// Ensure that $split errors when given more or less than two arguments.
pipeline = {
    $project: {split: {$split: ["$string"]}}
};
assertErrorCode(coll, pipeline, 16020);

pipeline = {
    $project: {split: {$split: ["$string", "b", "c"]}}
};
assertErrorCode(coll, pipeline, 16020);

// Ensure that $split errors when given non-string input.
assert(coll.drop());
assert.commandWorked(coll.insert({string: "abc"}));

pipeline = {
    $project: {split: {$split: [1, "$string"]}}
};
assertErrorCode(coll, pipeline, 40085);

pipeline = {
    $project: {split: {$split: ["$string", 1]}}
};
assertErrorCode(coll, pipeline, 40086);

// Ensure that $split errors when given an empty separator.
pipeline = {
    $project: {split: {$split: ["$string", ""]}}
};
assertErrorCode(coll, pipeline, 40087);
})();
