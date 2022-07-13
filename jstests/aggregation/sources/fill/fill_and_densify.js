/**
 * Test that $densify and $fill work together.
 * @tags: [
 *   requires_fcv_52,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/fixture_helpers.js");
load("jstests/libs/feature_flag_util.js");    // For isEnabled.
load("jstests/aggregation/extras/utils.js");  // For arrayEq.

const coll = db[jsTestName()];
coll.drop();

// Basic test.
let documents = [
    {_id: 1, val: 0, toFill: 1},
    {_id: 2, val: 5, toFill: 3},
    {_id: 3, val: 10, toFill: 5},
];

assert.commandWorked(coll.insert(documents));

let pipeline = [
    {$project: {_id: 0}},
    {$densify: {field: "val", range: {step: 1, bounds: "full"}}},
    {$fill: {output: {toFill: {method: "locf"}}}}
];

let result = coll.aggregate(pipeline).toArray();

let expected = [
    {val: 0, toFill: 1},
    {val: 1, toFill: 1},
    {val: 2, toFill: 1},
    {val: 3, toFill: 1},
    {val: 4, toFill: 1},
    {val: 5, toFill: 3},
    {val: 6, toFill: 3},
    {val: 7, toFill: 3},
    {val: 8, toFill: 3},
    {val: 9, toFill: 3},
    {val: 10, toFill: 5},
];

assertArrayEq({actual: result, expected: expected});

// Same test with partitions.
coll.drop();
documents = [
    {_id: 1, val: 0, toFill: 1, part: 1},
    {_id: 2, val: 5, toFill: 3, part: 1},
    {_id: 3, val: 10, toFill: 5, part: 1},
    {_id: 4, val: 3, toFill: 10, part: 2},
    {_id: 5, val: 6, toFill: 13, part: 2},
    {_id: 6, val: 9, toFill: 16, part: 2},
];

assert.commandWorked(coll.insert(documents));
pipeline = [
    {$project: {_id: 0}},
    {$densify: {field: "val", range: {step: 1, bounds: "partition"}, partitionByFields: ["part"]}},
    {$fill: {output: {toFill: {method: "locf"}}, partitionByFields: ["part"]}}
];
result = coll.aggregate(pipeline).toArray();

expected = [
    {val: 0, toFill: 1, part: 1},
    {val: 1, toFill: 1, part: 1},
    {val: 2, toFill: 1, part: 1},
    {val: 3, toFill: 1, part: 1},
    {val: 4, toFill: 1, part: 1},
    {val: 5, toFill: 3, part: 1},
    {val: 6, toFill: 3, part: 1},
    {val: 7, toFill: 3, part: 1},
    {val: 8, toFill: 3, part: 1},
    {val: 9, toFill: 3, part: 1},
    {val: 10, toFill: 5, part: 1},
    {val: 3, toFill: 10, part: 2},
    {val: 4, toFill: 10, part: 2},
    {val: 5, toFill: 10, part: 2},
    {val: 6, toFill: 13, part: 2},
    {val: 7, toFill: 13, part: 2},
    {val: 8, toFill: 13, part: 2},
    {val: 9, toFill: 16, part: 2},
];
assertArrayEq({actual: result, expected: expected});

pipeline = [
    {$project: {_id: 0}},
    {$densify: {field: "val", range: {step: 1, bounds: "full"}, partitionByFields: ["part"]}},
    {$fill: {output: {toFill: {method: "locf"}}, partitionByFields: ["part"]}}
];
result = coll.aggregate(pipeline).toArray();

expected = [
    {"val": 0, "toFill": 1, "part": 1},    {"part": 1, "val": 1, "toFill": 1},
    {"part": 1, "val": 2, "toFill": 1},    {"part": 1, "val": 3, "toFill": 1},
    {"part": 1, "val": 4, "toFill": 1},    {"val": 5, "toFill": 3, "part": 1},
    {"part": 1, "val": 6, "toFill": 3},    {"part": 1, "val": 7, "toFill": 3},
    {"part": 1, "val": 8, "toFill": 3},    {"part": 1, "val": 9, "toFill": 3},
    {"val": 10, "toFill": 5, "part": 1},   {"part": 2, "val": 0, "toFill": null},
    {"part": 2, "val": 1, "toFill": null}, {"part": 2, "val": 2, "toFill": null},
    {"val": 3, "toFill": 10, "part": 2},   {"part": 2, "val": 4, "toFill": 10},
    {"part": 2, "val": 5, "toFill": 10},   {"val": 6, "toFill": 13, "part": 2},
    {"part": 2, "val": 7, "toFill": 13},   {"part": 2, "val": 8, "toFill": 13},
    {"val": 9, "toFill": 16, "part": 2},   {"part": 2, "val": 10, "toFill": 16}
];
assertArrayEq({actual: result, expected: expected});

coll.drop();
documents = [
    {_id: 1, val: 0, toFill: 1, possible: 2, part: 1},
    {_id: 2, val: 5, toFill: 3, possible: null, part: 1},
    {_id: 3, val: 10, toFill: 5, possible: 17, part: 1},
    {_id: 4, val: 3, toFill: 10, possible: 1, part: 2},
    {_id: 5, val: 6, toFill: 13, possible: null, part: 2},
    {_id: 6, val: 9, toFill: 16, possible: 8, part: 2},
];

assert.commandWorked(coll.insert(documents));
pipeline = [
    {$project: {_id: 0}},
    {$densify: {field: "val", range: {step: 1, bounds: "partition"}, partitionByFields: ["part"]}},
    {
        $fill: {
            sortBy: {val: 1},
            output: {toFill: {method: "locf"}, possible: {method: "linear"}},
            partitionByFields: ["part"]
        }
    }
];
result = coll.aggregate(pipeline).toArray();
expected = [
    {"val": 0, "toFill": 1, "possible": 2, "part": 1},
    {"part": 1, "val": 1, "toFill": 1, "possible": 3.5},
    {"part": 1, "val": 2, "toFill": 1, "possible": 5},
    {"part": 1, "val": 3, "toFill": 1, "possible": 6.5},
    {"part": 1, "val": 4, "toFill": 1, "possible": 8},
    {"val": 5, "toFill": 3, "possible": 9.5, "part": 1},
    {"part": 1, "val": 6, "toFill": 3, "possible": 11},
    {"part": 1, "val": 7, "toFill": 3, "possible": 12.5},
    {"part": 1, "val": 8, "toFill": 3, "possible": 14},
    {"part": 1, "val": 9, "toFill": 3, "possible": 15.5},
    {"val": 10, "toFill": 5, "possible": 17, "part": 1},
    {"val": 3, "toFill": 10, "possible": 1, "part": 2},
    {"part": 2, "val": 4, "toFill": 10, "possible": 2.166666666666667},
    {"part": 2, "val": 5, "toFill": 10, "possible": 3.3333333333333335},
    {"val": 6, "toFill": 13, "possible": 4.5, "part": 2},
    {"part": 2, "val": 7, "toFill": 13, "possible": 5.666666666666667},
    {"part": 2, "val": 8, "toFill": 13, "possible": 6.833333333333334},
    {"val": 9, "toFill": 16, "possible": 8, "part": 2}
];
assertArrayEq({actual: result, expected: expected});
})();
