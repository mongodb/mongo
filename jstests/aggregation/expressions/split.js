// In SERVER-6773, the $split expression was introduced. In this file, we test the functionality and
// error cases of the expression.

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode and testExpression.
load("jstests/libs/sbe_assert_error_override.js");
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

// TODO SERVER-58095: When the classic engine is used, it will eagerly return null values, even
// if some of its arguments are invalid. This is not the case when SBE is enabled because of the
// order in which arguments are evaluated. In certain cases, errors will be thrown or the empty
// string will be returned instead of null.
const sbeEnabled = checkSBEEnabled(db);
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

// Ensure that $split operates correctly when the string has embedded null bytes.
testExpression(coll, {$split: ["a\0b\0c", "\0"]}, ["a", "b", "c"]);
testExpression(coll, {$split: ["\0a\0", "a"]}, ["\0", "\0"]);
testExpression(coll, {$split: ["\0\0\0", "a"]}, ["\0\0\0"]);

// Ensure that $split operates correctly when the string has multi-byte tokens or input strings.
testExpression(coll, {$split: ["∫a∫", "a"]}, ["∫", "∫"]);
testExpression(coll, {$split: ["a∫∫a", "∫"]}, ["a", "", "a"]);

// Ensure that $split produces null when given null as input.
testExpression(coll, {$split: ["abc", null]}, null);
testExpression(coll, {$split: [null, "abc"]}, null);

// Ensure that $split produces null when given missing fields as input.
testExpression(coll, {$split: ["$a", "a"]}, null);
testExpression(coll, {$split: ["a", "$a"]}, null);
testExpression(coll, {$split: ["$a", null]}, null);
testExpression(coll, {$split: [null, "$a"]}, null);
testExpression(coll, {$split: ["$missing", {$toLower: "$missing"}]}, null);

// SBE expression translation will detect our empty string, whereas the classic engine will
// detect that "$a" is missing and return null.
if (sbeEnabled) {
    testExpression(coll, {$split: ["", "$a"]}, [""]);
} else {
    testExpression(coll, {$split: ["", "$a"]}, null);
}

//
// Error Code tests with constant-folding optimization.
//

// Ensure that $split errors when given more or less than two arguments.
let pipeline = {$project: {split: {$split: []}}};
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

const stringNumericArg = {
    $split: [1, "$a"]
};
if (sbeEnabled) {
    pipeline = {$project: {split: stringNumericArg}};
    assertErrorCode(coll, pipeline, 40085);
} else {
    testExpression(coll, stringNumericArg, null);
}

const splitNumArg = {
    $split: ["$b", 1]
};
if (sbeEnabled) {
    pipeline = {$project: {split: splitNumArg}};
    assertErrorCode(coll, pipeline, 40086);
} else {
    testExpression(coll, splitNumArg, null);
}

const emptyStringDelim = {
    $split: ["$abc", ""]
};
if (sbeEnabled) {
    pipeline = {$project: {split: emptyStringDelim}};
    assertErrorCode(coll, pipeline, 40087);
} else {
    testExpression(coll, emptyStringDelim, null);
}
})();
