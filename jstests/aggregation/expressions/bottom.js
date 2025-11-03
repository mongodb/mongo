// Test $bottom expression.
/**
 * @tags: [
 *   requires_fcv_83
 * ]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

let coll = db.bottom_expr;
coll.drop();

// Need to have at least one document to ensure the pipeline executes
assert.commandWorked(coll.insert({}));

function testBottomNoSort(inputArray, expArray) {
    let pipeline = [{$project: {_id: 0, bottom: {$bottom: {input: inputArray}}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{bottom: expArray}]);
}

function testBottomWithSort(inputArray, sortBy, expArray) {
    let pipeline = [{$project: {_id: 0, bottom: {$bottom: {input: inputArray, sortBy: sortBy}}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{bottom: expArray}]);
}

// Testing without sort spec
testBottomNoSort([], null);
testBottomNoSort([0, 1, 2, 3, 4], 4);

// Testing mixed types
testBottomNoSort([0, 1, 2, 3, 4, "a", "b", "c"], "c");
testBottomNoSort([0, "a", [1, "b"], {c: 2}, 0.5], [1, "b"]);

// Testing with sort spec
testBottomWithSort([0, 1, 2, 3, 4], 1, 4);
testBottomWithSort([0, 1, 2, 3, 4], -1, 0);
testBottomWithSort([0, "a", [1, "b"], {c: 2}, 0.5], -1, 0);

// Testing nested types
testBottomWithSort(
    [
        [1, 2],
        [3, [1, 1]],
        [3, 6],
    ],
    1,
    [3, [1, 1]],
);
testBottomWithSort(
    [
        {a: 1, b: 1},
        {a: 2, b: 2},
        {a: 2, b: 1},
    ],
    {a: 1, b: -1}, // Ascending a then descending b
    {a: 2, b: 1},
);

// Error cases
// Bottom requires an object
assertErrorCode(coll, [{$project: {x: {$bottom: 0}}}], 7212122);
// Input is required
assertErrorCode(coll, [{$project: {x: {$bottom: {sortBy: 1}}}}], 7212124);

// Input argument is not an array
assertErrorCode(coll, [{$project: {x: {$bottom: {input: "one"}}}}], 721217);
assertErrorCode(coll, [{$project: {x: {$bottom: {input: 1}}}}], 721217);

// Sortby argument is not a sort spec
assertErrorCode(coll, [{$project: {x: {$bottom: {input: [1, 2, 3], sortBy: "a"}}}}], 2942507);
