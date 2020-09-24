// In SERVER-8951, $indexOfBytes was introduced. In this file, we test the correctness and error
// cases of the expression.
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode and testExpression.
load("jstests/libs/sbe_assert_error_override.js");

(function() {
"use strict";

function testExpressionBytes(coll, expression, result, shouldTestEquivalence = true) {
    testExpression(coll, expression, result);
    coll.drop();

    // Test sbe $indexOfBytes.
    const arr = expression.$indexOfBytes;
    let args = ['$string', '$substring'];
    if (arr.length == 3) {
        args = ['$string', '$substring', arr[2]];
    }
    if (arr.length == 4) {
        args = ['$string', '$substring', arr[2], arr[3]];
    }
    assert.commandWorked(coll.insert({string: arr[0], substring: arr[1]}));
    const aggResult =
        coll.aggregate({$project: {byteLocation: {$indexOfBytes: args}}}).toArray()[0];
    assert.eq(result, aggResult.byteLocation);
    coll.drop();

    if (shouldTestEquivalence) {
        // If we are specifying a starting or ending index for the search, we should be able to
        // achieve equivalent behavior using $substrBytes.
        var indexOfSpec = expression["$indexOfBytes"];
        var input = indexOfSpec[0];
        var token = indexOfSpec[1];
        var start = indexOfSpec.length > 2 ? indexOfSpec[2] : 0;
        // Use $strLenBytes because JavaScript's length property is based off of UTF-16, not the
        // actual number of bytes.
        var end = indexOfSpec.length > 3 ? indexOfSpec[3] : {$strLenBytes: input};

        var substrExpr = {
            $indexOfBytes: [{$substrBytes: [input, start, {$subtract: [end, start]}]}, token]
        };

        // Since the new expression takes the index with respect to a shortened string, the
        // output index will differ from the index with respect to the full length string,
        // unless the output is -1.
        var substrResult = (result === -1) ? -1 : result - start;

        testExpression(coll, substrExpr, substrResult);
    }
}

const coll = db.indexofbytes;
coll.drop();
assert.commandWorked(coll.insert({item: 'foobar foobar'}));

// Test that $indexOfBytes throws an error when given a string or substring that is not a string.
assert.commandFailedWithCode(
    assert.throws(() =>
                      coll.aggregate([{$project: {byteLocation: {$indexOfBytes: [4, '$item']}}}])),
                 40091);
assert.commandFailedWithCode(
    assert.throws(() =>
                      coll.aggregate([{$project: {byteLocation: {$indexOfBytes: ['$item', 4]}}}])),
                 40092);
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      [{$project: {byteLocation: {$indexOfBytes: ['$item', null]}}}])),
                 40092);
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      [{$project: {byteLocation: {$indexOfBytes: ['$item', '$missing']}}}])),
                 40092);

// Test that $indexOfBytes throws an error when given an invalid index.
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      [{$project: {byteLocation: {$indexOfBytes: ['$item', 'bar', 'hello']}}}])),
                 40096);
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      [{$project: {byteLocation: {$indexOfBytes: ['$item', 'bar', -2]}}}])),
                 40097);
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      [{$project: {byteLocation: {$indexOfBytes: ['$item', 'bar', 1, 'hello']}}}])),
                 40096);
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      [{$project: {byteLocation: {$indexOfBytes: ['$item', 'bar', 1, -2]}}}])),
                 40097);
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      [{$project: {byteLocation: {$indexOfBytes: ['$item', 'bar', 1.4]}}}])),
                 40096);
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      [{$project: {byteLocation: {$indexOfBytes: ['$item', 'bar', 1, 5.2]}}}])),
                 40096);

// Test that $indexOfBytes returns null when the first argument is null or missing.
assert.eq(null,
          coll.aggregate({$project: {byteLocation: {$indexOfBytes: [null, '$item']}}})
              .toArray()[0]
              .byteLocation);
assert.eq(null,
          coll.aggregate({$project: {byteLocation: {$indexOfBytes: ['$missing', '$item']}}})
              .toArray()[0]
              .byteLocation);
assert.eq(null,
          coll.aggregate({$project: {byteLocation: {$indexOfBytes: [undefined, '$item']}}})
              .toArray()[0]
              .byteLocation);

// Test that $indexOfBytes returns null when given a string or substring that is not a string.
assert.eq(null,
          coll.aggregate({$project: {byteLocation: {$indexOfBytes: ['$missing', null]}}})
              .toArray()[0]
              .byteLocation);
assert.eq(null,
          coll.aggregate({$project: {byteLocation: {$indexOfBytes: ['$missing', 4]}}})
              .toArray()[0]
              .byteLocation);
assert.eq(null,
          coll.aggregate({$project: {byteLocation: {$indexOfBytes: ['$missing', '$missing']}}})
              .toArray()[0]
              .byteLocation);
assert.eq(null,
          coll.aggregate({$project: {byteLocation: {$indexOfBytes: ['$missing', undefined]}}})
              .toArray()[0]
              .byteLocation);
coll.drop();

// Test that $indexOfBytes works with standard strings and substrings.
testExpressionBytes(coll, {$indexOfBytes: ['foobar foobar', 'bar']}, 3, false);
testExpressionBytes(coll, {$indexOfBytes: ['foobar foobar', 'bar', 5]}, 10, false);
testExpressionBytes(coll, {$indexOfBytes: ['foobar foobar', 'foo', 1, 5]}, -1, false);

// Test that $indexOfBytes returns -1 when the substring is not within bounds.
testExpressionBytes(coll, {$indexOfBytes: ['foobar foobar', 'bar', 0, 2]}, -1, false);
testExpressionBytes(coll, {$indexOfBytes: ['foobar foobar', 'zzz']}, -1, false);
testExpressionBytes(coll, {$indexOfBytes: ['foobar foobar', 'zzz', 10]}, -1, false);
testExpressionBytes(coll, {$indexOfBytes: ['foobar foobar', 'zzz', 0, 20]}, -1, false);

// Test that $indexOfBytes works with indexes of different numeric types.
testExpressionBytes(coll, {$indexOfBytes: ['foobar foobar', 'bar', 5.0]}, 10, false);
testExpressionBytes(coll, {$indexOfBytes: ['foobar foobar', 'foo', 1.0, 5.0]}, -1, false);

// Test that $indexOfBytes returns -1 when given poorly defined bounds.
testExpressionBytes(coll, {$indexOfBytes: ['foobar foobar', 'bar', 20]}, -1, false);
testExpressionBytes(coll, {$indexOfBytes: ['foobar foobar', 'bar', 4, 1]}, -1, false);

// Test that $indexOfBytes works for the edge case of both string and substring being empty.
testExpressionBytes(coll, {$indexOfBytes: ["", ""]}, 0, false);

testExpressionBytes(coll, {$indexOfBytes: ["abc", "b"]}, 1);

testExpressionBytes(coll, {$indexOfBytes: ["abcba", "b"]}, 1);

testExpressionBytes(coll, {$indexOfBytes: ["abc", "d"]}, -1);

testExpressionBytes(coll, {$indexOfBytes: ["abcba", "b", 2]}, 3);

testExpressionBytes(coll, {$indexOfBytes: ["abcde", "d", 0, 2]}, -1);

testExpressionBytes(coll, {$indexOfBytes: ["abc", "b", 1]}, 1);

testExpressionBytes(coll, {$indexOfBytes: ["abc", "b", 0, 10]}, 1);

testExpressionBytes(coll, {$indexOfBytes: ["abcbabc", "b", 2, 4]}, 3);

// $strLenBytes does not accept null as an input.
testExpressionBytes(coll, {$indexOfBytes: [null, "b"]}, null, false);

testExpressionBytes(coll, {$indexOfBytes: ["abc", "b", 3]}, -1);

testExpressionBytes(coll, {$indexOfBytes: ["abc", "b", 3, 1]}, -1);

testExpressionBytes(coll, {$indexOfBytes: ["abc", "b", 3, 5]}, -1);

testExpressionBytes(coll, {$indexOfBytes: ["", " "]}, -1);

testExpressionBytes(coll, {$indexOfBytes: [" ", ""]}, 0);

testExpressionBytes(coll, {$indexOfBytes: ["", ""]}, 0);

testExpressionBytes(coll, {$indexOfBytes: ["abc", "", 3]}, 3);

testExpressionBytes(coll, {$indexOfBytes: ["abc", "", 1]}, 1);

// Test with multi-byte tokens.

testExpressionBytes(coll, {$indexOfBytes: ["abcde", "de"]}, 3);

testExpressionBytes(coll, {$indexOfBytes: ["abcde", "def"]}, -1);

// Test with non-ASCII characters. Some tests do not test equivalence using $substrBytes because
// $substrBytes disallows taking a substring that begins or ends in the middle of a UTF-8
// encoding of a character.
testExpressionBytes(coll, {$indexOfBytes: ["a∫∫b", "b"]}, 7);

// $substrBytes would attempt to take the substring from the middle of a UTF-8
// encoding of a character.
testExpressionBytes(coll, {$indexOfBytes: ["a∫∫b", "b", 6]}, 7, false);

testExpressionBytes(coll, {$indexOfBytes: ["abc∫ba", "∫"]}, 3);

testExpressionBytes(coll, {$indexOfBytes: ["∫∫∫", "a"]}, -1);

// $substrBytes would attempt to take the substring from the middle of a UTF-8
// encoding of a character.
testExpressionBytes(coll, {$indexOfBytes: ["ab∫c", "c", 0, 3]}, -1, false);

testExpressionBytes(coll, {$indexOfBytes: ["abc∫b∫", "b∫"]}, 6);

// Test with embedded null bytes.
testExpressionBytes(coll, {$indexOfBytes: ["abc\0d", "d"]}, 4);

testExpressionBytes(coll, {$indexOfBytes: ["abc\0", "\0"]}, 3);

testExpressionBytes(coll, {$indexOfBytes: ["abc\0d\0", "d", 5, 6]}, -1);

// Error cases.

var pipeline = {
    $project: {
        output: {
            $indexOfBytes: [3, "s"],
        }
    }
};
assertErrorCode(coll, pipeline, 40091);

pipeline = {
    $project: {
        output: {
            $indexOfBytes: ["s", 3],
        }
    }
};
assertErrorCode(coll, pipeline, 40092);

pipeline = {
    $project: {output: {$indexOfBytes: ["abc", "b", "bad"]}}
};
assertErrorCode(coll, pipeline, 40096);

pipeline = {
    $project: {output: {$indexOfBytes: ["abc", "b", 0, "bad"]}}
};
assertErrorCode(coll, pipeline, 40096);

pipeline = {
    $project: {output: {$indexOfBytes: ["abc", "b", -1]}}
};
assertErrorCode(coll, pipeline, 40097);

pipeline = {
    $project: {output: {$indexOfBytes: ["abc", "b", 1, -1]}}
};
assertErrorCode(coll, pipeline, 40097);
}());
