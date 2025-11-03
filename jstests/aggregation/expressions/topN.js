// Test $topN expression.
/**
 * @tags: [
 *   requires_fcv_83
 * ]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

let coll = db.topN_expr;
coll.drop();

// Need to have at least one document to ensure the pipeline executes
assert.commandWorked(coll.insert({}));

function testTopNNoSort(n, inputArray, expArray) {
    let pipeline = [{$project: {_id: 0, topN: {$topN: {n: n, input: inputArray}}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{topN: expArray}]);
}

function testTopNWithSort(n, inputArray, sortBy, expArray) {
    let pipeline = [{$project: {_id: 0, topN: {$topN: {n: n, input: inputArray, sortBy: sortBy}}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{topN: expArray}]);
}

// Testing without sort spec

testTopNNoSort(0, [0, 1, 2, 3, 4], []);
testTopNNoSort(2, [0, 1, 2, 3, 4], [0, 1]);
testTopNNoSort(5, [0, 1, 2, 3, 4], [0, 1, 2, 3, 4]);
testTopNNoSort(10, [0, 1, 2, 3, 4], [0, 1, 2, 3, 4]);

// Testing mixed types
testTopNNoSort(2, [0, 1, 2, 3, 4, "a", "b", "c"], [0, 1]);
testTopNNoSort(2, [0, "a", [1, "b"], {c: 2}, 0.5], [0, 0.5]);

// Testing with sort spec
testTopNWithSort(2, [0, 1, 2, 3, 4], 1, [0, 1]);
testTopNWithSort(2, [0, 1, 2, 3, 4], -1, [4, 3]);
testTopNWithSort(2, [0, "a", [1, "b"], {c: 2}, 0.5], -1, [[1, "b"], {c: 2}]);

// Testing nested types
testTopNWithSort(
    2,
    [
        [1, 2],
        [3, [1, 1]],
        [3, 6],
    ],
    1,
    [
        [1, 2],
        [3, 6],
    ],
);
testTopNWithSort(
    2,
    [
        {a: 1, b: 1},
        {a: 2, b: 2},
        {a: 2, b: 1},
    ],
    {a: 1, b: -1}, // Ascending a then descending b
    [
        {a: 1, b: 1},
        {a: 2, b: 2},
    ],
);

// Error cases
// TopN requires an object
assertErrorCode(coll, [{$project: {x: {$topN: 0}}}], 721218);
// N is required
assertErrorCode(coll, [{$project: {x: {$topN: {input: [1, 2, 3]}}}}], 7212110);
// Input is required
assertErrorCode(coll, [{$project: {x: {$topN: {n: 2}}}}], 7212111);

// First argument is not integral
assertErrorCode(coll, [{$project: {x: {$topN: {n: "2", input: [1, 2, 3]}}}}], 721210);
assertErrorCode(coll, [{$project: {x: {$topN: {n: 1.5, input: [1, 2, 3]}}}}], 721210);
// First argument is negative
assertErrorCode(coll, [{$project: {x: {$topN: {n: -1, input: [1, 2, 3]}}}}], 721212);

// Second argument is not an array
assertErrorCode(coll, [{$project: {x: {$topN: {n: 2, input: "one"}}}}], 721211);
assertErrorCode(coll, [{$project: {x: {$topN: {n: 2, input: 1}}}}], 721211);

// Third argument is not a sort spec
assertErrorCode(coll, [{$project: {x: {$topN: {n: 2, input: [1, 2, 3], sortBy: "a"}}}}], 2942507);
