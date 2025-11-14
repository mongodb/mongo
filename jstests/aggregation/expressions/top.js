// Test $top expression.
/**
 * @tags: [
 *   requires_fcv_83
 * ]
 */
import "jstests/libs/query/sbe_assert_error_override.js";

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

let coll = db.top_expr;
coll.drop();

// Need to have at least one document to ensure the pipeline executes
assert.commandWorked(coll.insert({}));

function testTopNoSort(inputArray, expArray) {
    let pipeline = [{$project: {_id: 0, top: {$top: {input: inputArray}}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{top: expArray}]);
}

function testTopWithSort(inputArray, sortBy, expArray) {
    let pipeline = [{$project: {_id: 0, top: {$top: {input: inputArray, sortBy: sortBy}}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{top: expArray}]);
}

// Testing without sort spec
testTopNoSort([], null);
testTopNoSort([0, 1, 2, 3, 4], 0);

// Testing mixed types
testTopNoSort([0, 1, 2, 3, 4, "a", "b", "c"], 0);
testTopNoSort([0, "a", [1, "b"], {c: 2}, 0.5], 0);

// Testing with sort spec
testTopWithSort([0, 1, 2, 3, 4], 1, 0);
testTopWithSort([0, 1, 2, 3, 4], -1, 4);
testTopWithSort([0, "a", [1, "b"], {c: 2}, 0.5], -1, [1, "b"]);

// Testing nested types
testTopWithSort(
    [
        [1, 2],
        [3, [1, 1]],
        [3, 6],
    ],
    1,
    [1, 2],
);
testTopWithSort(
    [
        {a: 1, b: 1},
        {a: 2, b: 2},
        {a: 2, b: 1},
    ],
    {a: 1, b: -1}, // Ascending a then descending b
    {a: 1, b: 1},
);

// Error cases
// Top requires an object
assertErrorCode(coll, [{$project: {x: {$top: 0}}}], 7212113);
// Input is required
assertErrorCode(coll, [{$project: {x: {$top: {sortBy: 1}}}}], 7212115);

// Input argument is not an array
assertErrorCode(coll, [{$project: {x: {$top: {input: "one"}}}}], 721213);
assertErrorCode(coll, [{$project: {x: {$top: {input: 1}}}}], 721213);

// Sortby argument is not a sort spec
assertErrorCode(coll, [{$project: {x: {$top: {input: [1, 2, 3], sortBy: "a"}}}}], 2942507);
