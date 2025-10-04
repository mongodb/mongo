// In SERVER-8951, $indexOfArray was introduced. In this file, we test the correctness and error
// cases of the expression.
import "jstests/libs/query/sbe_assert_error_override.js";

import {assertErrorCode, testExpression} from "jstests/aggregation/extras/utils.js";

let coll = db.indexofarray;
coll.drop();

// Insert a dummy document to ensure something flows through the pipeline.
assert.commandWorked(coll.insert({}));

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

let pipeline = {
    $project: {
        output: {
            $indexOfArray: ["string", "s"],
        },
    },
};
assertErrorCode(coll, pipeline, 40090);

pipeline = {
    $project: {output: {$indexOfArray: [[1, 2, 3], 2, "bad"]}},
};
assertErrorCode(coll, pipeline, 40096);

pipeline = {
    $project: {output: {$indexOfArray: [[1, 2, 3], 2, 0, "bad"]}},
};
assertErrorCode(coll, pipeline, 40096);

pipeline = {
    $project: {output: {$indexOfArray: [[1, 2, 3], 2, -1]}},
};
assertErrorCode(coll, pipeline, 40097);

pipeline = {
    $project: {output: {$indexOfArray: [[1, 2, 3], 2, 1, -1]}},
};
assertErrorCode(coll, pipeline, 40097);
