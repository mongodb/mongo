/**
 * Test that $setUnion works as a window function.
 * @tags: [featureFlagArrayAccumulators, requires_fcv_81]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db["set_window_fields_set_union"];
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0, notAnArray: 'a', vals: [1, 2, 3]},
    {_id: 1, notAnArray: 'a', vals: [2, 3, 4]},
    {_id: 2, notAnArray: 'a', vals: [2, 3, 4]},
    {_id: 3, notAnArray: 'a', vals: [5, 6]},
    {_id: 4, notAnArray: 'a', vals: []}
]));

// Test that $setUnion does not produce duplicate values in its output. We sort the output array
// because $setUnion does not give any guarantees on ordering.
let result = coll.aggregate([
                     {$setWindowFields: {sortBy: {_id: 1}, output: {v: {$setUnion: '$vals'}}}},
                     {$sort: {_id: 1}},
                     {$project: {_id: 1, setUnionArr: {$sortArray: {input: '$v', sortBy: 1}}}}
                 ])
                 .toArray();

assert.eq(result, [
    {_id: 0, setUnionArr: [1, 2, 3, 4, 5, 6]},
    {_id: 1, setUnionArr: [1, 2, 3, 4, 5, 6]},
    {_id: 2, setUnionArr: [1, 2, 3, 4, 5, 6]},
    {_id: 3, setUnionArr: [1, 2, 3, 4, 5, 6]},
    {_id: 4, setUnionArr: [1, 2, 3, 4, 5, 6]}
]);

// Test that $setUnion respects the window boundaries (i.e. that removal works). The test data has
// been written such that there are arrays with overlapping values. These will be contained in the
// same window, ensuring that we are testing that we only remove one occurrence of each value. For
// example, if '2' has been inserted into the window twice and gets removed once, '2' will still be
// in the output array until it gets removed a second time.
result = coll.aggregate([
                 {
                     $setWindowFields: {
                         sortBy: {_id: 1},
                         output: {v: {$setUnion: '$vals', window: {documents: [-1, 0]}}}
                     }
                 },
                 {$sort: {_id: 1}},
                 {$project: {_id: 1, setUnionArr: {$sortArray: {input: '$v', sortBy: 1}}}}
             ])
             .toArray();

assert.eq(result, [
    {_id: 0, setUnionArr: [1, 2, 3]},
    {_id: 1, setUnionArr: [1, 2, 3, 4]},
    {_id: 2, setUnionArr: [2, 3, 4]},
    {_id: 3, setUnionArr: [2, 3, 4, 5, 6]},
    {_id: 4, setUnionArr: [5, 6]}
]);

// Test for errors on non-array types ($setUnion only supports arrays).
assertErrorCode(coll,
                [{$setWindowFields: {output: {willFail: {$setUnion: '$notAnArray'}}}}],
                ErrorCodes.TypeMismatch);
