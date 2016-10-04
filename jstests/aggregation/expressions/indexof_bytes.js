// In SERVER-8951, $indexOfBytes was introduced. In this file, we test the correctness and error
// cases of the expression.
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode and testExpression.

(function() {
    "use strict";

    function testExpressionBytes(coll, expression, result, shouldTestEquivalence = true) {
        testExpression(coll, expression, result);

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

    var coll = db.indexofbytes;
    coll.drop();

    // Insert a dummy document so something flows through the pipeline.
    assert.writeOK(coll.insert({}));

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

    pipeline = {$project: {output: {$indexOfBytes: ["abc", "b", "bad"]}}};
    assertErrorCode(coll, pipeline, 40096);

    pipeline = {$project: {output: {$indexOfBytes: ["abc", "b", 0, "bad"]}}};
    assertErrorCode(coll, pipeline, 40096);

    pipeline = {$project: {output: {$indexOfBytes: ["abc", "b", -1]}}};
    assertErrorCode(coll, pipeline, 40097);

    pipeline = {$project: {output: {$indexOfBytes: ["abc", "b", 1, -1]}}};
    assertErrorCode(coll, pipeline, 40097);
}());
