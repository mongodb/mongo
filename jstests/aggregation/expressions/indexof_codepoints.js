// In SERVER-8951, $indexOfCP was introduced. In this file, we test the correctness and error
// cases of the expression.
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode and testExpression.

(function() {
    "use strict";

    function testExpressionCodePoints(coll, expression, result, shouldTestEquivalence = true) {
        testExpression(coll, expression, result);

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

    var coll = db.indexofcp;
    coll.drop();

    // Insert a dummy document so something flows through the pipeline.
    assert.writeOK(coll.insert({}));

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

    pipeline = {$project: {output: {$indexOfCP: ["abc", "b", "bad"]}}};
    assertErrorCode(coll, pipeline, 40096);

    pipeline = {$project: {output: {$indexOfCP: ["abc", "b", 0, "bad"]}}};
    assertErrorCode(coll, pipeline, 40096);

    pipeline = {$project: {output: {$indexOfCP: ["abc", "b", -1]}}};
    assertErrorCode(coll, pipeline, 40097);

    pipeline = {$project: {output: {$indexOfCP: ["abc", "b", 1, -1]}}};
    assertErrorCode(coll, pipeline, 40097);
}());
