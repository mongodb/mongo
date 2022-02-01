/**
 * Test that $locf works as a window function.
 * @tags: [
 *   # Needed as $fill and $locf are 5.2 features.
 *   requires_fcv_52,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");
load("jstests/aggregation/extras/utils.js");  // For arrayEq.
load("jstests/libs/feature_flag_util.js");    // For isEnabled.

const coll = db[jsTestName()];
coll.drop();

// Test that $locf doesn't parse with a window.
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $setWindowFields: {
            sortBy: {_id: 1},
            output: {val: {$locf: {}, window: []}},
        }
    }],
    cursor: {}
}),
                             ErrorCodes.FailedToParse);

// Create some documents.
let collection = [
    {_id: 0, val: null},
    {_id: 1, val: 0},
    {_id: 2, val: 2},
    {_id: 3, val: null},
    {_id: 4},
    {_id: 5, val: "str"},
    {_id: 6, val: null},
    {_id: 7, rand: "rand"},
];
assert.commandWorked(coll.insert(collection));

let result = coll.aggregate([{
                     $setWindowFields: {
                         sortBy: {_id: 1},
                         output: {val: {$locf: "$val"}},
                     }
                 }])
                 .toArray();
let expected = [
    {_id: 0, val: null},
    {_id: 1, val: 0},
    {_id: 2, val: 2},
    {_id: 3, val: 2},
    {_id: 4, val: 2},
    {_id: 5, val: "str"},
    {_id: 6, val: "str"},
    {_id: 7, rand: "rand", val: "str"},
];
assertArrayEq({actual: result, expected: expected});

// Test projecting to a different field.
result = coll.aggregate([{
                 $setWindowFields: {
                     sortBy: {_id: 1},
                     output: {newVal: {$locf: "$val"}},
                 }

             }])
             .toArray();

expected = [
    {_id: 0, val: null, newVal: null},
    {_id: 1, val: 0, newVal: 0},
    {_id: 2, val: 2, newVal: 2},
    {_id: 3, val: null, newVal: 2},
    {_id: 4, newVal: 2},
    {_id: 5, val: "str", newVal: "str"},
    {_id: 6, val: null, newVal: "str"},
    {_id: 7, rand: "rand", newVal: "str"},
];
assertArrayEq({actual: result, expected: expected});

// Partitions don't mix values
collection = [
    {_id: 1, val: 0, part: 1},
    {_id: 2, val: 2, part: 2},
    {_id: 3, val: null, part: 1},
    {_id: 4, val: null, part: 2},
];
coll.drop();
assert.commandWorked(coll.insert(collection));

result = coll.aggregate([{
                 $setWindowFields: {
                     sortBy: {_id: 1},
                     output: {val: {$locf: "$val"}},
                     partitionBy: "$part",
                 }
             }])
             .toArray();

expected = [
    {_id: 1, val: 0, part: 1},
    {_id: 2, val: 2, part: 2},
    {_id: 3, val: 0, part: 1},
    {_id: 4, val: 2, part: 2},
];
assertArrayEq({actual: result, expected: expected});

// Defaults to null even with missing values.
collection = [
    {_id: 1},
    {_id: 2},
    {_id: 3, val: null},
    {_id: 4, val: null},
];
coll.drop();
assert.commandWorked(coll.insert(collection));
expected = [
    {_id: 1, val: null},
    {_id: 2, val: null},
    {_id: 3, val: null},
    {_id: 4, val: null},
];

result = coll.aggregate([{
                 $setWindowFields: {
                     sortBy: {_id: 1},
                     output: {val: {$locf: "$val"}},
                 }
             }])
             .toArray();

assertArrayEq({actual: result, expected: expected});

collection = [
    {_id: 1, val: null},
    {_id: 2},
    {_id: 3, val: null},
    {_id: 4},
];
coll.drop();
assert.commandWorked(coll.insert(collection));

result = coll.aggregate([{
                 $setWindowFields: {
                     sortBy: {_id: 1},
                     output: {val: {$locf: "$val"}},
                 }
             }])
             .toArray();

assertArrayEq({actual: result, expected: expected});
})();
