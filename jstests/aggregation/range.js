/*
 * Tests general $range functionality.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");
load("jstests/libs/sbe_assert_error_override.js");

const coll = db.range;
coll.drop();

assert.commandWorked(coll.insert([
    {city: "San Jose", distance: NumberInt(42)},
    {city: "Sacramento", distance: NumberInt(88)},
    {city: "Reno", distance: NumberInt(218)},
    {city: "Los Angeles", distance: NumberInt(383)},
]));

const positiveRangeExpectedResult = [
    {"city": "San Jose", "Rest stops": [0, 25]},
    {"city": "Sacramento", "Rest stops": [0, 25, 50, 75]},
    {"city": "Reno", "Rest stops": [0, 25, 50, 75, 100, 125, 150, 175, 200]},
    {
        "city": "Los Angeles",
        "Rest stops": [0, 25, 50, 75, 100, 125, 150, 175, 200, 225, 250, 275, 300, 325, 350, 375]
    },
];

// Expecting the results to have an "Rest stops" array with positive elements.
const positiveRangeResult =
    coll.aggregate([{
            $project: {
                _id: 0,
                city: 1,
                "Rest stops": {$range: [NumberInt(0), "$distance", NumberInt(25)]}
            }
        }])
        .toArray();

assert(arrayEq(positiveRangeExpectedResult, positiveRangeResult));

// Expecting same result when Int64 is used as long as it's value is representable in Int32.
const positiveRangeResult2 =
    coll.aggregate([{
            $project: {
                _id: 0,
                city: 1,
                "Rest stops": {$range: [NumberLong(0), "$distance", NumberLong(25)]}
            }
        }])
        .toArray();

assert(arrayEq(positiveRangeExpectedResult, positiveRangeResult2));

const negativeRangeExpectedResult = [
    {"city": "San Jose", "Rest stops": [0, -25]},
    {"city": "Sacramento", "Rest stops": [0, -25, -50, -75]},
    {"city": "Reno", "Rest stops": [0, -25, -50, -75, -100, -125, -150, -175, -200]},
    {
        "city": "Los Angeles",
        "Rest stops": [
            0,
            -25,
            -50,
            -75,
            -100,
            -125,
            -150,
            -175,
            -200,
            -225,
            -250,
            -275,
            -300,
            -325,
            -350,
            -375
        ]
    },
];

// Expecting the results to have an "Rest stops" array with negative elements.
const negativeRangeResult =
    coll.aggregate([{
            $project: {
                _id: 0,
                city: 1,
                "Rest stops":
                    {$range: [NumberInt(0), {"$multiply": ["$distance", -1]}, NumberInt(-25)]}
            }
        }])
        .toArray();

assert(arrayEq(negativeRangeExpectedResult, negativeRangeResult));

const nothingRangeExpectedResult = [
    {"city": "San Jose", "Rest stops": []},
    {"city": "Sacramento", "Rest stops": []},
    {"city": "Reno", "Rest stops": []},
    {"city": "Los Angeles", "Rest stops": []},
];

// Expecting the results to have an empty "Rest stops" array.
const nothingRangeResult =
    coll.aggregate([{
            $project: {
                _id: 0,
                city: 1,
                "Rest stops":
                    {$range: [NumberInt(0), {"$multiply": ["$distance", -1]}, NumberInt(25)]}
            }
        }])
        .toArray();

assert(arrayEq(nothingRangeExpectedResult, nothingRangeResult));

// Expecting the results to have an empty "Rest stops" array.
const nothingRangeResult2 =
    coll.aggregate([{
            $project:
                {_id: 0, city: 1, "Rest stops": {$range: ["$distance", "$distance", NumberInt(25)]}}
        }])
        .toArray();

assert(arrayEq(nothingRangeExpectedResult, nothingRangeResult2));

// Testing default step.
coll.drop();

assert.commandWorked(coll.insert([
    {city: "San Jose", distance: NumberInt(5)},
    {city: "Sacramento", distance: NumberInt(8)},
    {city: "Reno", distance: NumberInt(2)},
    {city: "Los Angeles", distance: NumberInt(1)},
]));

const rangeDefaultStepExpectedResult = [
    {"city": "San Jose", "Rest stops": [0, 1, 2, 3, 4]},
    {"city": "Sacramento", "Rest stops": [0, 1, 2, 3, 4, 5, 6, 7]},
    {"city": "Reno", "Rest stops": [0, 1]},
    {"city": "Los Angeles", "Rest stops": [0]},
];

// Expecting the results to have an "Rest stops" array with positive elements starting from 0 to the
// distance value.
const rangeDefaultStepResult =
    coll.aggregate(
            [{$project: {_id: 0, city: 1, "Rest stops": {$range: [NumberInt(0), "$distance"]}}}])
        .toArray();

assert(arrayEq(rangeDefaultStepExpectedResult, rangeDefaultStepResult));

// Expecting the results to have an empty "Rest stops" array.
const nothingRangeDefaultStepResult =
    coll.aggregate([{
            $project: {
                _id: 0,
                city: 1,
                "Rest stops": {$range: [NumberInt(0), {"$multiply": ["$distance", NumberInt(-1)]}]}
            }
        }])
        .toArray();

assert(arrayEq(nothingRangeExpectedResult, nothingRangeDefaultStepResult));

// Expecting the results to have an empty "Rest stops" array.
const nothingRangeDefaultStepResult2 =
    coll.aggregate(
            [{$project: {_id: 0, city: 1, "Rest stops": {$range: ["$distance", "$distance"]}}}])
        .toArray();

assert(arrayEq(nothingRangeExpectedResult, nothingRangeDefaultStepResult2));

coll.drop();

assert.commandWorked(coll.insert([
    {city: "San Jose", distance: NumberInt(100)},
]));

// Testing overflow errors due to $range start and end taking int32 values.
// Example: {$range: [100, 2147483647, 1073741824]}
// Output will OOM because array will look like this:
// [ 100, 1073741924, -2147483548, -1073741724, 100, 1073741924, -2147483548, -1073741724, 100, … so
// on and so forth ]
const overflowRangeExpectedResult = [{"city": "San Jose", "Rest stops": [100, 1073741924]}];

const overflowRangeResult =
    coll.aggregate([{
            $project: {
                _id: 0,
                city: 1,
                "Rest stops": {$range: ["$distance", NumberInt(2147483647), NumberInt(1073741824)]}
            }
        }])
        .toArray();

assert(arrayEq(overflowRangeExpectedResult, overflowRangeResult));

// Testing int32 representable errors (Arguments to $range must be int32 representable).
let pipeline;

// Start value is too big.
pipeline = [
    {$project: {_id: 0, city: 1, "Rest stops": {$range: [NumberLong("12147483647"), "$distance"]}}}
];
assertErrorCode(coll, pipeline, 34444);

// Start value is a decimal.
pipeline =
    [{$project: {_id: 0, city: 1, "Rest stops": {$range: [NumberDecimal("0.35"), "$distance"]}}}];
assertErrorCode(coll, pipeline, 34444);

// Start value is not a number.
pipeline = [
    {$project: {_id: 0, city: 1, "Rest stops": {$range: ["String is not a number", "$distance"]}}}
];
assertErrorCode(coll, pipeline, 34443);

// Start value is null.
pipeline = [{$project: {_id: 0, city: 1, "Rest stops": {$range: [null, "$distance"]}}}];
assertErrorCode(coll, pipeline, 34443);

// End value is too big.
pipeline = [
    {$project: {_id: 0, city: 1, "Rest stops": {$range: ["$distance", NumberLong("12147483647")]}}}
];
assertErrorCode(coll, pipeline, 34446);

// End value is a decimal.
pipeline =
    [{$project: {_id: 0, city: 1, "Rest stops": {$range: ["$distance", NumberDecimal("0.35")]}}}];
assertErrorCode(coll, pipeline, 34446);

// End value is not a number.
pipeline = [
    {$project: {_id: 0, city: 1, "Rest stops": {$range: ["$distance", "String is not a number"]}}}
];
assertErrorCode(coll, pipeline, 34445);

// End value is null.
pipeline = [{$project: {_id: 0, city: 1, "Rest stops": {$range: ["$distance", null]}}}];
assertErrorCode(coll, pipeline, 34445);

// Step value is too big.
pipeline = [{
    $project: {
        _id: 0,
        city: 1,
        "Rest stops": {$range: ["$distance", NumberInt(100), NumberLong("12147483647")]}
    }
}];
assertErrorCode(coll, pipeline, 34448);

// Step value is a decimal.
pipeline = [{
    $project: {
        _id: 0,
        city: 1,
        "Rest stops": {$range: ["$distance", NumberInt(100), NumberDecimal("0.35")]}
    }
}];
assertErrorCode(coll, pipeline, 34448);

// Step value is not a number.
pipeline = [{
    $project: {
        _id: 0,
        city: 1,
        "Rest stops": {$range: ["$distance", NumberInt(100), "String is not a number"]}
    }
}];
assertErrorCode(coll, pipeline, 34447);

// Step value is null.
pipeline =
    [{$project: {_id: 0, city: 1, "Rest stops": {$range: ["$distance", NumberInt(100), null]}}}];
assertErrorCode(coll, pipeline, 34447);

// Step value is zero.
pipeline = [{$project: {_id: 0, city: 1, "Rest stops": {$range: [0, "$distance", 0]}}}];
assertErrorCode(coll, pipeline, 34449);

// Testing decimal values that are Int32 representable.
const decimalRangeExpectedResult = [{"city": "San Jose", "Rest stops": [100, 200]}];

// Expecting results same as if decimals were Int32.
const decimalRangeResult =
    coll.aggregate([{
            $project: {
                _id: 0,
                city: 1,
                "Rest stops":
                    {$range: ["$distance", NumberDecimal("201.0"), NumberDecimal("100.0")]}
            }
        }])
        .toArray();

assert(arrayEq(decimalRangeExpectedResult, decimalRangeResult));

assert(coll.drop());
assert.commandWorked(coll.insertOne({_id: 1}));
assertErrorCode(
    coll, [{$project: {result: {$range: [0, 1073741924]}}}], ErrorCodes.ExceededMemoryLimit);
assert(arrayEq([{_id: 1, result: []}],
               coll.aggregate([{$project: {result: {$range: [0, 1073741924, -1]}}}]).toArray()));
}());
