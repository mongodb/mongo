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

if (!FeatureFlagUtil.isEnabled(db, "Fill")) {
    jsTestLog("Skipping as featureFlagFill is not enabled");
    return;
}

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
assertArrayEq(result, expected);

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
assertArrayEq(result, expected);

// Values stay missing if all values are missing.
collection = [
    {_id: 1},
    {_id: 2},
    {_id: 3, val: null},
    {_id: 4, val: null},
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

assertArrayEq(result, collection);

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

assertArrayEq(result, collection);
})();
