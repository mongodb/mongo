// In SERVER-8951, $indexOfCP was introduced. In this file, we test the correctness and error
// cases of the expression.
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode and testExpression.
load("jstests/libs/sbe_assert_error_override.js");

(function() {
"use strict";

function testExpressionCodePoints(coll, expression, result, shouldTestEquivalence = true) {
    testExpression(coll, expression, result);
    coll.drop();

    // Test sbe $indexOfCP.
    const arr = expression.$indexOfCP;
    let args = ['$string', '$substring'];
    if (arr.length == 3) {
        args = ['$string', '$substring', arr[2]];
    }
    if (arr.length == 4) {
        args = ['$string', '$substring', arr[2], arr[3]];
    }
    assert.commandWorked(coll.insert({string: arr[0], substring: arr[1]}));
    const aggResult = coll.aggregate({$project: {byteLocation: {$indexOfCP: args}}}).toArray()[0];
    assert.eq(result, aggResult.byteLocation);
    coll.drop();

    var indexOfSpec = expression["$indexOfCP"];
    if (shouldTestEquivalence) {
        // If we are specifying a starting or ending index for the search, we should be able to
        // achieve equivalent behavior using $substrCP.
        var input = indexOfSpec[0];
        var token = indexOfSpec[1];
        var start = indexOfSpec.length > 2 ? indexOfSpec[2] : 0;
        var end = indexOfSpec.length > 3 ? indexOfSpec[3] : {$strLenCP: input};

        var substrExpr = {
            $indexOfCP: [{$substrCP: [input, start, {$subtract: [end, start]}]}, token]
        };

        // Since the new expression takes the index with respect to a shortened string, the
        // output index will differ from the index with respect to the full length string,
        // unless the output is -1.
        var substrResult = (result === -1) ? -1 : result - start;

        testExpression(coll, substrExpr, substrResult);
    }
}

const coll = db.indexofcp;
coll.drop();
assert.commandWorked(coll.insert({item: 'foobar foobar'}));

// Test that $indexOfCP throws an error when given a string or substring that is not a string.
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate([{$project: {byteLocation: {$indexOfCP: [4, '$item']}}}])),
                 40093);
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate([{$project: {byteLocation: {$indexOfCP: ['$item', 4]}}}])),
                 40094);
assert.commandFailedWithCode(
    assert.throws(() =>
                      coll.aggregate([{$project: {byteLocation: {$indexOfCP: ['$item', null]}}}])),
                 40094);
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      [{$project: {byteLocation: {$indexOfCP: ['$item', '$missing']}}}])),
                 40094);

// Test that $indexOfCP throws an error when given an invalid index.
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      [{$project: {byteLocation: {$indexOfCP: ['$item', 'bar', 'hello']}}}])),
                 40096);
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      [{$project: {byteLocation: {$indexOfCP: ['$item', 'bar', -2]}}}])),
                 40097);
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      [{$project: {byteLocation: {$indexOfCP: ['$item', 'bar', 1, 'hello']}}}])),
                 40096);
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      [{$project: {byteLocation: {$indexOfCP: ['$item', 'bar', 1, -2]}}}])),
                 40097);
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      [{$project: {byteLocation: {$indexOfCP: ['$item', 'bar', 1.4]}}}])),
                 40096);
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      [{$project: {byteLocation: {$indexOfCP: ['$item', 'bar', 1, 5.2]}}}])),
                 40096);

// Test that $indexOfCP returns null when the first argument is null or missing.
assert.eq(null,
          coll.aggregate({$project: {byteLocation: {$indexOfCP: [null, '$item']}}})
              .toArray()[0]
              .byteLocation);
assert.eq(null,
          coll.aggregate({$project: {byteLocation: {$indexOfCP: ['$missing', '$item']}}})
              .toArray()[0]
              .byteLocation);
assert.eq(null,
          coll.aggregate({$project: {byteLocation: {$indexOfCP: [undefined, '$item']}}})
              .toArray()[0]
              .byteLocation);

// Test that $indexOfCP returns null when given a string or substring that is not a string.
assert.eq(null,
          coll.aggregate({$project: {byteLocation: {$indexOfCP: ['$missing', null]}}})
              .toArray()[0]
              .byteLocation);
assert.eq(null,
          coll.aggregate({$project: {byteLocation: {$indexOfCP: ['$missing', 4]}}})
              .toArray()[0]
              .byteLocation);
assert.eq(null,
          coll.aggregate({$project: {byteLocation: {$indexOfCP: ['$missing', '$missing']}}})
              .toArray()[0]
              .byteLocation);
coll.drop();

// Test that $indexOfCP works with ASCII strings and substrings.
testExpressionCodePoints(coll, {$indexOfCP: ['foobar foobar', 'bar']}, 3, false);
testExpressionCodePoints(coll, {$indexOfCP: ['foobar foobar', 'bar', 5]}, 10, false);
testExpressionCodePoints(coll, {$indexOfCP: ['foobar foobar', 'foo', 1, 5]}, -1, false);

// Test that $indexOfCP returns -1 when the substring is not within bounds.
testExpressionCodePoints(coll, {$indexOfCP: ['foobar foobar', 'bar', 0, 2]}, -1, false);
testExpressionCodePoints(coll, {$indexOfCP: ['foobar foobar', 'zzz']}, -1, false);
testExpressionCodePoints(coll, {$indexOfCP: ['foobar foobar', 'zzz', 10]}, -1, false);
testExpressionCodePoints(coll, {$indexOfCP: ['foobar foobar', 'zzz', 0, 20]}, -1, false);

// Test that $indexOfCP works with indexes of different numeric types.
testExpressionCodePoints(coll, {$indexOfCP: ['foobar foobar', 'bar', 5.0]}, 10, false);
testExpressionCodePoints(coll, {$indexOfCP: ['foobar foobar', 'foo', 1.0, 5.0]}, -1, false);

// Test that $indexOfCP returns -1 when given poorly defined bounds.
testExpressionCodePoints(coll, {$indexOfCP: ['foobar foobar', 'bar', 20]}, -1, false);
testExpressionCodePoints(coll, {$indexOfCP: ['foobar foobar', 'bar', 4, 1]}, -1, false);

// Test that $indexOfCP works for the edge case of both string and substring being empty.
testExpressionCodePoints(coll, {$indexOfCP: ["", ""]}, 0, false);

// Test that $indexOfCP works with strings with codepoints of different byte sizes.
testExpressionCodePoints(coll, {$indexOfCP: ['\u039C\u039FNG\u039F', 'NG']}, 2, false);
testExpressionCodePoints(coll, {$indexOfCP: ['\u039C\u039FNG\u039F', '\u039F', 2]}, 4, false);

// Test that $indexOfCP works with strings with codepoints of different sizes.
testExpressionCodePoints(coll, {$indexOfCP: ['cafétéria', 'é']}, 3, false);
testExpressionCodePoints(coll, {$indexOfCP: ['cafétéria', 't']}, 4, false);
testExpressionCodePoints(coll, {$indexOfCP: ['cafétéria', 'é', 4]}, 5, false);
testExpressionCodePoints(coll, {$indexOfCP: ['cafétéria', 'é', 6]}, -1, false);
testExpressionCodePoints(coll, {$indexOfCP: ['cafétéria', 'a', 3, 5]}, -1, false);

testExpressionCodePoints(coll, {$indexOfCP: ["∫aƒ", "ƒ"]}, 2);

testExpressionCodePoints(coll, {$indexOfCP: ["a∫c", "d"]}, -1);

testExpressionCodePoints(coll, {$indexOfCP: ["∫b∫ba", "b", 2]}, 3);

testExpressionCodePoints(coll, {$indexOfCP: ["ab∫de", "d", 0, 3]}, -1);

testExpressionCodePoints(coll, {$indexOfCP: ["ab∫de", "d", 0, 4]}, 3);

testExpressionCodePoints(coll, {$indexOfCP: ["øøc", "ø", 1]}, 1);

testExpressionCodePoints(coll, {$indexOfCP: ["øƒc", "ƒ", 0, 10]}, 1);

testExpressionCodePoints(coll, {$indexOfCP: ["abcbabc", "b", 2, 4]}, 3);

// $strLenCP does not accept null as an input.
testExpressionCodePoints(coll, {$indexOfCP: [null, "√"]}, null, false);

testExpressionCodePoints(coll, {$indexOfCP: ["abc", "b", 3]}, -1);

// We are intentionally testing specifying an end index before the start index, which is why we
// cannot use $substrCP in checking for equivalence.
testExpressionCodePoints(coll, {$indexOfCP: ["a√cb", "b", 3, 1]}, -1, false);

testExpressionCodePoints(coll, {$indexOfCP: ["a∫b", "b", 3, 5]}, -1);

testExpressionCodePoints(coll, {$indexOfCP: ["", "∫"]}, -1);

testExpressionCodePoints(coll, {$indexOfCP: [" ", ""]}, 0);

testExpressionCodePoints(coll, {$indexOfCP: ["", ""]}, 0);

testExpressionCodePoints(coll, {$indexOfCP: ["abc", "", 1]}, 1);

// Test with multi-byte tokens.

testExpressionCodePoints(coll, {$indexOfCP: ["abcƒe", "ƒe"]}, 3);

testExpressionCodePoints(coll, {$indexOfCP: ["∫aeøø", "øøø"]}, -1);

// Test with embedded null bytes.

testExpressionCodePoints(coll, {$indexOfCP: ["ab∫\0d", "d"]}, 4);

testExpressionCodePoints(coll, {$indexOfCP: ["øbc\0", "\0"]}, 3);

testExpressionCodePoints(coll, {$indexOfCP: ["πbƒ\0d\0", "d", 5, 6]}, -1);

// Error cases.

var pipeline = {
    $project: {
        output: {
            $indexOfCP: [3, "s"],
        }
    }
};
assertErrorCode(coll, pipeline, 40093);

pipeline = {
    $project: {
        output: {
            $indexOfCP: ["s", 3],
        }
    }
};
assertErrorCode(coll, pipeline, 40094);

pipeline = {
    $project: {output: {$indexOfCP: ["abc", "b", "bad"]}}
};
assertErrorCode(coll, pipeline, 40096);

pipeline = {
    $project: {output: {$indexOfCP: ["abc", "b", 0, "bad"]}}
};
assertErrorCode(coll, pipeline, 40096);

pipeline = {
    $project: {output: {$indexOfCP: ["abc", "b", -1]}}
};
assertErrorCode(coll, pipeline, 40097);

pipeline = {
    $project: {output: {$indexOfCP: ["abc", "b", 1, -1]}}
};
assertErrorCode(coll, pipeline, 40097);
}());
