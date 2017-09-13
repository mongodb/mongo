// In SERVER-8951, $indexOfArray was introduced. In this file, we test the correctness and error
// cases of the expression.
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode and testExpression.

(function() {
    "use strict";

    var coll = db.indexofarray;
    coll.drop();

    // Insert a dummy document to ensure something flows through the pipeline.
    assert.writeOK(coll.insert({}));

    testExpression(coll, {$indexOfArray: [[1, 2, 3], 2]}, 1);

    testExpression(coll, {$indexOfArray: [[1, 2, 3], 4]}, -1);

    testExpression(coll, {$indexOfArray: [[1, 2, 3, 2, 1], 2, 2]}, 3);

    testExpression(coll, {$indexOfArray: [[1, 2, 3, 4, 5], 4, 0, 3]}, -1);

    testExpression(coll, {$indexOfArray: [[1, 2, 3], 2, 1]}, 1);

    testExpression(coll, {$indexOfArray: [[1, 2, 3], 2, 0, 10]}, 1);

    testExpression(coll, {$indexOfArray: [[1, 2, 3, 2, 1, 2, 3], 2, 2, 4]}, 3);

    testExpression(coll, {$indexOfArray: [null, 2]}, null);

    testExpression(coll, {$indexOfArray: [[1, 2, 3], 2, 3]}, -1);

    testExpression(coll, {$indexOfArray: [[1, 2, 3], 2, 3, 1]}, -1);

    testExpression(coll, {$indexOfArray: [[1, 2, 3], 2, 3, 3]}, -1);

    testExpression(coll, {$indexOfArray: [[1, 2, 3], 2, 3, 5]}, -1);

    testExpression(coll, {$indexOfArray: [[], 1]}, -1);

    var pipeline = {
        $project: {
            output: {
                $indexOfArray: ["string", "s"],
            }
        }
    };
    assertErrorCode(coll, pipeline, 40090);

    pipeline = {$project: {output: {$indexOfArray: [[1, 2, 3], 2, "bad"]}}};
    assertErrorCode(coll, pipeline, 40096);

    pipeline = {$project: {output: {$indexOfArray: [[1, 2, 3], 2, 0, "bad"]}}};
    assertErrorCode(coll, pipeline, 40096);

    pipeline = {$project: {output: {$indexOfArray: [[1, 2, 3], 2, -1]}}};
    assertErrorCode(coll, pipeline, 40097);

    pipeline = {$project: {output: {$indexOfArray: [[1, 2, 3], 2, 1, -1]}}};
    assertErrorCode(coll, pipeline, 40097);
}());
