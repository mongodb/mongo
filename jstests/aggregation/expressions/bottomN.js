// Test $bottomN expression.
/**
 * @tags: [
 *   requires_fcv_83
 * ]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

let coll = db.bottomN_expr;
coll.drop();

// Need to have at least one document to ensure the pipeline executes
assert.commandWorked(coll.insert({}));

function testBottomNNoSort(n, inputArray, expArray) {
    let pipeline = [{$project: {_id: 0, bottomN: {$bottomN: {n: n, input: inputArray}}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{bottomN: expArray}]);
}

function testBottomNWithSort(n, inputArray, sortBy, expArray) {
    let pipeline = [{$project: {_id: 0, bottomN: {$bottomN: {n: n, input: inputArray, sortBy: sortBy}}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{bottomN: expArray}]);
}

// Testing without sort spec
testBottomNNoSort(0, [0, 1, 2, 3, 4], []);
testBottomNNoSort(2, [0, 1, 2, 3, 4], [3, 4]);
testBottomNNoSort(5, [0, 1, 2, 3, 4], [0, 1, 2, 3, 4]);
testBottomNNoSort(10, [0, 1, 2, 3, 4], [0, 1, 2, 3, 4]);

// Testing mixed types
testBottomNNoSort(2, [0, 1, 2, 3, 4, "a", "b", "c"], ["b", "c"]);
testBottomNNoSort(2, [0, "a", [1, "b"], {c: 2}, 0.5], [{c: 2}, [1, "b"]]);

// Testing with sort spec
testBottomNWithSort(2, [0, 1, 2, 3, 4], 1, [3, 4]);
testBottomNWithSort(2, [0, 1, 2, 3, 4], -1, [1, 0]);
testBottomNWithSort(2, [0, "a", [1, "b"], {c: 2}, 0.5], -1, [0.5, 0]);

// Testing nested types
testBottomNWithSort(
    2,
    [
        [1, 2],
        [3, [1, 1]],
        [3, 6],
    ],
    1,
    [
        [3, 6],
        [3, [1, 1]],
    ],
);
testBottomNWithSort(
    2,
    [
        {a: 1, b: 1},
        {a: 2, b: 2},
        {a: 2, b: 1},
    ],
    {a: 1, b: -1}, // Ascending a then descending b
    [
        {a: 2, b: 2},
        {a: 2, b: 1},
    ],
);

// Error cases
// BottomN requires an object
assertErrorCode(coll, [{$project: {x: {$bottomN: 0}}}], 7212117);
// N is required
assertErrorCode(coll, [{$project: {x: {$bottomN: {input: [1, 2, 3]}}}}], 7212119);
// Input is required
assertErrorCode(coll, [{$project: {x: {$bottomN: {n: 2}}}}], 7212120);

// First argument is not integral
assertErrorCode(coll, [{$project: {x: {$bottomN: {n: "2", input: [1, 2, 3]}}}}], 721214);
assertErrorCode(coll, [{$project: {x: {$bottomN: {n: 1.5, input: [1, 2, 3]}}}}], 721214);
// First argument is negative
assertErrorCode(coll, [{$project: {x: {$bottomN: {n: -1, input: [1, 2, 3]}}}}], 721216);

// Second argument is not an array
assertErrorCode(coll, [{$project: {x: {$bottomN: {n: 2, input: "one"}}}}], 721215);
assertErrorCode(coll, [{$project: {x: {$bottomN: {n: 2, input: 1}}}}], 721215);

// Third argument is not a sort spec
assertErrorCode(coll, [{$project: {x: {$bottomN: {n: 2, input: [1, 2, 3], sortBy: "a"}}}}], 2942507);
