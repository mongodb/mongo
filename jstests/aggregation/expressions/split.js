// In SERVER-6773, the $split expression was introduced. In this file, we test the functionality and
// error cases of the expression.
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode and testExpression.

(function() {
    "use strict";

    var coll = db.split;
    coll.drop();
    assert.writeOK(coll.insert({}));

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
    // Note that this expression is not unicode-aware; splitting is based wholly off of the byte
    // sequence of the input and token.
    testExpression(coll, {$split: ["∫a∫", "a"]}, ["∫", "∫"]);
    testExpression(coll, {$split: ["a∫∫a", "∫"]}, ["a", "", "a"]);

    // Ensure that $split produces null when given null as input.
    testExpression(coll, {$split: ["abc", null]}, null);
    testExpression(coll, {$split: [null, "abc"]}, null);

    // Ensure that $split produces null when given missing fields as input.
    testExpression(coll, {$split: ["$a", "a"]}, null);
    testExpression(coll, {$split: ["a", "$a"]}, null);

    // Ensure that $split errors when given more or less than two arguments.
    var pipeline = {$project: {split: {$split: []}}};
    assertErrorCode(coll, pipeline, 16020);

    pipeline = {$project: {split: {$split: ["a"]}}};
    assertErrorCode(coll, pipeline, 16020);

    pipeline = {$project: {split: {$split: ["a", "b", "c"]}}};
    assertErrorCode(coll, pipeline, 16020);

    // Ensure that $split errors when given non-string input.
    pipeline = {$project: {split: {$split: [1, "abc"]}}};
    assertErrorCode(coll, pipeline, 40085);

    pipeline = {$project: {split: {$split: ["abc", 1]}}};
    assertErrorCode(coll, pipeline, 40086);

    // Ensure that $split errors when given an empty separator.
    pipeline = {$project: {split: {$split: ["abc", ""]}}};
    assertErrorCode(coll, pipeline, 40087);
}());
