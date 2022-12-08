/**
 * Test that $densify works with Decimal128 type values.
 * @tags: [
 *   # Needed as $densify is a 51 feature.
 *   requires_fcv_51,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq.

function buildErrorString(found, expected) {
    return "Expected:\n" + tojson(expected) + "\nGot:\n" + tojson(found);
}

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert([
    {val: NumberDecimal(0)},
    {val: NumberDecimal(10)},
]));

let pipeline = [{$project: {_id: 0}}, {$densify: {field: "val", range: {step: 1, bounds: "full"}}}];

let expectedResult = [];
for (let i = 0; i <= 10; i++) {
    expectedResult.push({val: NumberDecimal(i)});
}

let result = coll.aggregate(pipeline).toArray();

assert(arrayEq(result, expectedResult), buildErrorString(result, expectedResult));

// Add a double in the middle of the range.
assert.commandWorked(coll.insert({val: 5}));
expectedResult = [];
// $densify uses the last type seen for generated documents.
for (let i = 0; i <= 10; i++) {
    if (i < 5 || i == 10) {
        expectedResult.push({val: NumberDecimal(i)});
    } else {
        expectedResult.push({val: i});
    }
}
result = coll.aggregate(pipeline).toArray();

assert(arrayEq(result, expectedResult), buildErrorString(result, expectedResult));

// Run the same test, but with a Decimal step.
pipeline = [
    {$project: {_id: 0}},
    {$densify: {field: "val", range: {step: NumberDecimal(1), bounds: "full"}}}
];
expectedResult = [];
for (let i = 0; i <= 10; i++) {
    if (i != 5) {
        expectedResult.push({val: NumberDecimal(i)});
    } else {
        expectedResult.push({val: i});
    }
}
result = coll.aggregate(pipeline).toArray();

assert(arrayEq(result, expectedResult), buildErrorString(result, expectedResult));

// Run the same test, but with decimals instead of integers.
coll.drop();
assert.commandWorked(coll.insert([
    {val: NumberDecimal(0)},
    {val: .5},
    {val: NumberDecimal(1)},
]));
pipeline = [{$project: {_id: 0}}, {$densify: {field: "val", range: {step: .1, bounds: "full"}}}];
// Note that all the results after .5 may not be precisely on the step, but instead be off by a
// vanishingly small amount.
expectedResult = [
    {val: NumberDecimal(0)},
    {val: NumberDecimal(".1")},
    {val: NumberDecimal(".2")},
    {val: NumberDecimal(".3")},
    {val: NumberDecimal(".4")},
    {val: .5},
    {val: .6},
    {val: .7},
    {val: .7999999999999999},
    {val: .8999999999999999},
    {val: .9999999999999999},
    {val: NumberDecimal(1)},
];
result = coll.aggregate(pipeline).toArray();
assert(arrayEq(result, expectedResult), buildErrorString(result, expectedResult));

// Repeat with a NumberDecimal step
pipeline = [
    {$project: {_id: 0}},
    {$densify: {field: "val", range: {step: NumberDecimal(".1"), bounds: "full"}}}
];
expectedResult = [
    {val: NumberDecimal(0)},
    {val: NumberDecimal(".1")},
    {val: NumberDecimal(".2")},
    {val: NumberDecimal(".3")},
    {val: NumberDecimal(".4")},
    {val: .5},
    {val: NumberDecimal(".6")},
    {val: NumberDecimal(".7")},
    {val: NumberDecimal(".8")},
    {val: NumberDecimal(".9")},
    {val: NumberDecimal(1)},
];
result = coll.aggregate(pipeline).toArray();

assert(arrayEq(result, expectedResult), buildErrorString(result, expectedResult));

// If the step is Decimal128, return Decimal128.
coll.drop();
assert.commandWorked(coll.insert([
    {val: 0},
    {val: 1},
]));

pipeline = [
    {$project: {_id: 0}},
    {$densify: {field: "val", range: {step: NumberDecimal(.001), bounds: "full"}}},
    // No need to check every value.
    {$limit: 2},
];
result = coll.aggregate(pipeline).toArray();
assert(arrayEq(result, [{val: 0}, {val: NumberDecimal(.001)}]));

// Decimal bounds fail if step is not decimal.
pipeline = [
    {$project: {_id: 0}},
    {$densify: {field: "val", range: {step: .1, bounds: [NumberDecimal(.1), NumberDecimal(.9)]}}},
    // No need to check every value.
    {$limit: 3},
];
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}), 5876900);

// Verify that if 'step' is not representable as a double, precision is not lost during computation.
const preciseStep = NumberDecimal(".1243568735894448377382");
const preciseStepTimesTwo = NumberDecimal(".2487137471788896754764");
const preciseStepTimesThree = NumberDecimal(".3730706207683345132146");
pipeline = [
    {$project: {_id: 0}},
    {$densify: {field: "val", range: {step: preciseStep, bounds: "full"}}},
    // No need to check every value.
    {$limit: 4},
];
result = coll.aggregate(pipeline).toArray();
expectedResult = [
    {val: 0},
    {val: preciseStep},
    {val: preciseStepTimesTwo},
    {val: preciseStepTimesThree},
];
assert(arrayEq(result, expectedResult), buildErrorString(result, expectedResult));
})();
