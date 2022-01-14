/**
 * Test that densify works for partitions.
 * @tags: [
 *   # Needed as $densify is a 51 feature.
 *   requires_fcv_51,
 * ]
 */

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // arrayEq

const coll = db[jsTestName()];

function buildErrorString(found, expected) {
    return "Expected:\n" + tojson(expected) + "\nGot:\n" + tojson(found);
}

// Two docs in each of two partitions.
function testOne() {
    coll.drop();

    const testDocs = [
        {val: 0, partition: 0},
        {val: 2, partition: 0},
        {val: 0, partition: 1},
        {val: 2, partition: 1}
    ];
    assert.commandWorked(coll.insert(testDocs));

    let result = coll.aggregate([
        {$project: {_id: 0}},
        {
            $densify: {
                field: "val",
                partitionByFields: ["partition"],
                range: {step: 1, bounds: "partition"}
            }
        }
    ]);
    const resultArray = result.toArray();
    const testExpected = testDocs.concat([{val: 1, partition: 0}, {val: 1, partition: 1}]);
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
    coll.drop();
}

function testOneDates() {
    coll.drop();

    const testDocs = [
        {val: new ISODate("2021-01-01"), partition: 0},
        {val: new ISODate("2021-01-03"), partition: 0},
        {val: new ISODate("2021-01-01"), partition: 1},
        {val: new ISODate("2021-01-03"), partition: 1}
    ];
    assert.commandWorked(coll.insert(testDocs));

    let result = coll.aggregate([
        {$project: {_id: 0}},
        {
            $densify: {
                field: "val",
                partitionByFields: ["partition"],
                range: {step: 1, unit: "day", bounds: "partition"}
            }
        }
    ]);
    const resultArray = result.toArray();
    const testExpected = testDocs.concat([
        {val: new ISODate("2021-01-02"), partition: 0},
        {val: new ISODate("2021-01-02"), partition: 1}
    ]);
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
    coll.drop();
}

// Same as test one, but partitions are interleaved.
function testTwo() {
    coll.drop();
    const testDocs = [
        {val: 0, partition: 0},
        {val: 0, partition: 1},
        {val: 2, partition: 1},
        {val: 2, partition: 0}
    ];
    assert.commandWorked(coll.insert(testDocs));

    let result = coll.aggregate([
        {$project: {_id: 0}},
        {
            $densify: {
                field: "val",
                partitionByFields: ["partition"],
                range: {step: 1, bounds: "partition"}
            }
        }
    ]);
    const resultArray = result.toArray();
    const testExpected = testDocs.concat([{val: 1, partition: 0}, {val: 1, partition: 1}]);
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
    coll.drop();
}

// Two larger partitions, interleaved.
function testThree() {
    coll.drop();
    let testDocs = [];
    let testExpected = [];
    for (let i = 0; i < 20; i++) {
        for (let j = 0; j < 2; j++) {
            if (i % 4 == 2 && j == 0) {
                testDocs.push({val: i, part: j});
            } else if (i % 5 == 0 && j == 1) {
                testDocs.push({val: i, part: j});
            }
            // Should have every document below 16 in first partition and 15 in the second.
            if (i >= 2 && i <= 18 && j == 0) {
                testExpected.push({val: i, part: j});
            }
            if (i <= 15 && j == 1) {
                testExpected.push({val: i, part: j});
            }
        }
    }
    assert.commandWorked(coll.insert(testDocs));
    let result = coll.aggregate([
        {$project: {_id: 0}},
        {
            $densify:
                {field: "val", partitionByFields: ["part"], range: {step: 1, bounds: "partition"}}
        }
    ]);
    const resultArray = result.toArray();
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
}

// Five small partitions.
function testFour() {
    coll.drop();
    let testDocs = [];
    let testExpected = [];
    for (let partVal = 0; partVal < 5; partVal++) {
        // Add an initial document to each partition.
        testDocs.push({val: 0, part: partVal});
        testExpected.push({val: 0, part: partVal});
        for (let densifyVal = 1; densifyVal < 10; densifyVal++) {
            if (partVal > 0 && densifyVal % partVal == 0) {
                testDocs.push({val: densifyVal, part: partVal});
            }
            testExpected.push({val: densifyVal, part: partVal});
        }
        // Add a top document to each partition.
        testDocs.push({val: 10, part: partVal});
        testExpected.push({val: 10, part: partVal});
    }
    assert.commandWorked(coll.insert(testDocs));
    let result = coll.aggregate([
        {$project: {_id: 0}},
        {
            $densify:
                {field: "val", partitionByFields: ["part"], range: {step: 1, bounds: "partition"}}
        }
    ]);
    const resultArray = result.toArray();
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
}

// One partition doesn't need densifying.
function testFive() {
    coll.drop();

    const testDocs = [
        {val: 0, partition: 0},
        {val: 2, partition: 0},
        {val: 123, partition: 1},
    ];
    assert.commandWorked(coll.insert(testDocs));

    let result = coll.aggregate([
        {$project: {_id: 0}},
        {
            $densify: {
                field: "val",
                partitionByFields: ["partition"],
                range: {step: 1, bounds: "partition"}
            }
        }
    ]);
    const resultArray = result.toArray();
    const testExpected = testDocs.concat([{val: 1, partition: 0}]);
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
    coll.drop();
}

// Verify the following test works in the full case without partitions.
function fullTestOne(stepVal = 1) {
    coll.drop();
    let testDocs = [];
    let testExpected = [];
    // Add an initial document.
    testDocs.push({val: 0});
    testExpected.push({val: 0});
    for (let densifyVal = 1; densifyVal < 11; densifyVal++) {
        if (densifyVal % 2 == 0) {
            testDocs.push({val: densifyVal});
            testExpected.push({val: densifyVal});
        } else if (densifyVal % stepVal == 0) {
            testExpected.push({val: densifyVal});
        }
    }
    testDocs.push({val: 11});
    testExpected.push({val: 11});
    assert.commandWorked(coll.insert(testDocs));
    let result = coll.aggregate(
        [{$project: {_id: 0}}, {$densify: {field: "val", range: {step: stepVal, bounds: "full"}}}]);
    const resultArray = result.toArray();
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
}

// Multiple partition fields.
function testFive(stepVal = 1) {
    coll.drop();
    let testDocs = [];
    let testExpected = [];
    for (let partValA = 0; partValA < 2; partValA++) {
        for (let partValB = 0; partValB < 2; partValB++) {
            // Add an initial document to each partition.
            testDocs.push({val: 0, partA: partValA, partB: partValB});
            testExpected.push({val: 0, partA: partValA, partB: partValB});
            for (let densifyVal = 1; densifyVal < 11; densifyVal++) {
                if (densifyVal % 2 == 0) {
                    testDocs.push({val: densifyVal, partA: partValA, partB: partValB});
                    testExpected.push({val: densifyVal, partA: partValA, partB: partValB});
                } else if (densifyVal % stepVal == 0) {
                    testExpected.push({val: densifyVal, partA: partValA, partB: partValB});
                }
            }
            // Add a max document to each partition.
            testDocs.push({val: 11, partA: partValA, partB: partValB});
            testExpected.push({val: 11, partA: partValA, partB: partValB});
        }
    }
    assert.commandWorked(coll.insert(testDocs));
    let result = coll.aggregate([
        {$project: {_id: 0}},
        {
            $densify: {
                field: "val",
                partitionByFields: ["partA", "partB"],
                range: {step: stepVal, bounds: "partition"}
            }
        }
    ]);
    const resultArray = result.toArray();
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
}

// Test partitioning with full where partitions need to be densified at the end.
// Three partitions, each with only one document.
function fullTestTwo(stepVal = 2) {
    coll.drop();
    let testDocs = [];
    let testExpected = [];
    // Add an initial document.
    testDocs.push({val: 0, part: 0});
    testDocs.push({val: 0, part: 1});
    testDocs.push({val: 10, part: 2});
    for (let densifyVal = 0; densifyVal < 11; densifyVal += stepVal) {
        for (let partitionVal = 0; partitionVal <= 2; partitionVal++) {
            testExpected.push({val: densifyVal, part: partitionVal});
        }
    }
    assert.commandWorked(coll.insert(testDocs));
    let result = coll.aggregate([
        {$project: {_id: 0}},
        {
            $densify:
                {field: "val", range: {step: stepVal, bounds: "full"}, partitionByFields: ["part"]}
        },
        {$sort: {val: 1, part: 1}}
    ]);
    const resultArray = result.toArray();
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
}

// Test partitioning with dates with full where partitions need to be densified at the end.
// Three partitions, each with only one document.
function fullTestTwoDates(stepVal = 2) {
    coll.drop();
    let testDocs = [];
    let testExpected = [];
    // Add an initial document.
    testDocs.push({val: new ISODate("2021-01-01"), part: 0});
    testDocs.push({val: new ISODate("2021-01-01"), part: 1});
    testDocs.push({val: new ISODate("2031-01-01"), part: 2});
    testDocs.push({val: new ISODate("2025-01-01"), part: 3});
    for (let densifyVal = 0; densifyVal < 11; densifyVal += stepVal) {
        for (let partitionVal = 0; partitionVal <= 3; partitionVal++) {
            testExpected.push({
                val: new ISODate((2021 + densifyVal).toString().padStart(2, '0') + "-01-01"),
                part: partitionVal
            });
        }
    }
    assert.commandWorked(coll.insert(testDocs));
    let result = coll.aggregate([
        {$project: {_id: 0}},
        {
            $densify: {
                field: "val",
                range: {step: stepVal, unit: "year", bounds: "full"},
                partitionByFields: ["part"]
            }
        },
        {$sort: {val: 1, part: 1}}
    ]);
    const resultArray = result.toArray();
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
}

// Same as above, but with extra documents in the middle of each partition somewhere.
function fullTestThree(stepVal = 2) {
    coll.drop();
    let testDocs = [];
    let testExpected = [];
    // Add an initial document.
    testDocs.push({val: 0, part: 0});
    testDocs.push({val: 4, part: 0});
    testDocs.push({val: 0, part: 1});
    testDocs.push({val: 5, part: 1});
    testExpected.push({val: 5, part: 1});
    testDocs.push({val: 10, part: 2});
    for (let densifyVal = 0; densifyVal < 11; densifyVal += stepVal) {
        for (let partitionVal = 0; partitionVal <= 2; partitionVal++) {
            testExpected.push({val: densifyVal, part: partitionVal});
        }
    }
    assert.commandWorked(coll.insert(testDocs));
    let result = coll.aggregate([
        {$project: {_id: 0}},
        {
            $densify:
                {field: "val", range: {step: stepVal, bounds: "full"}, partitionByFields: ["part"]}
        },
        {$sort: {val: 1, part: 1}}
    ]);
    const resultArray = result.toArray();
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
}

// Two partitions with no documents in the range.
function rangeTestOne() {
    coll.drop();

    const testDocs = [
        {val: 0, partition: 0},
        {val: 4, partition: 1},
    ];

    const expectedDocs = [
        {val: 0, partition: 0},
        {val: 4, partition: 1},
        {val: 2, partition: 0},
        {val: 2, partition: 1},
    ];
    assert.commandWorked(coll.insert(testDocs));

    let result = coll.aggregate([
        {$project: {_id: 0}},
        {
            $densify:
                {field: "val", partitionByFields: ["partition"], range: {step: 1, bounds: [2, 3]}}
        }
    ]);
    const resultArray = result.toArray();
    assert(arrayEq(resultArray, expectedDocs), buildErrorString(resultArray, expectedDocs));
    coll.drop();
}

// Upper bound on range which is off-step from lower bound.
function rangeTestThree() {
    coll.drop();

    const testDocs = [
        {val: 0, partition: 0},
        {val: 10, partition: 1},
    ];

    const expectedDocs = [
        {val: 0, partition: 0},
        {val: 10, partition: 1},
        {val: 2, partition: 0},
        {val: 2, partition: 1},
        {val: 4, partition: 0},
        {val: 4, partition: 1},
    ];
    assert.commandWorked(coll.insert(testDocs));

    let result = coll.aggregate([
        {$project: {_id: 0}},
        {
            $densify:
                {field: "val", partitionByFields: ["partition"], range: {step: 2, bounds: [2, 5]}}
        }
    ]);
    const resultArray = result.toArray();
    assert(arrayEq(resultArray, expectedDocs), buildErrorString(resultArray, expectedDocs));
    coll.drop();
}

// Three partitions, each with different documents w/respect to the range.
function rangeTestTwo() {
    coll.drop();
    let testDocs = [];
    let testExpected = [];
    testDocs.push({val: 0, part: 0});
    testExpected.push({val: 0, part: 0});
    testDocs.push({val: 5, part: 1});
    testExpected.push({val: 5, part: 1});
    testDocs.push({val: 10, part: 2});
    testExpected.push({val: 10, part: 2});
    for (let densifyVal = 4; densifyVal < 8; densifyVal += 2) {
        for (let partitionVal = 0; partitionVal <= 2; partitionVal++) {
            testExpected.push({val: densifyVal, part: partitionVal});
        }
    }
    assert.commandWorked(coll.insert(testDocs));
    let result = coll.aggregate([
        {$project: {_id: 0}},
        {$densify: {field: "val", range: {step: 2, bounds: [4, 8]}, partitionByFields: ["part"]}},
        {$sort: {val: 1, part: 1}}
    ]);
    const resultArray = result.toArray();
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
}

function rangeTestTwoDates() {
    coll.drop();
    let testDocs = [];
    let testExpected = [];
    testDocs.push({val: new ISODate("2021-01-01"), part: 0});
    testExpected.push({val: new ISODate("2021-01-01"), part: 0});
    testDocs.push({val: new ISODate("2021-06-01"), part: 1});
    testExpected.push({val: new ISODate("2021-06-01"), part: 1});
    testDocs.push({val: new ISODate("2021-11-01"), part: 2});
    testExpected.push({val: new ISODate("2021-11-01"), part: 2});
    for (let densifyVal = 4; densifyVal < 8; densifyVal += 2) {
        for (let partitionVal = 0; partitionVal <= 2; partitionVal++) {
            testExpected.push({
                val: new ISODate("2021-" + densifyVal.toString().padStart(2, '0') + "-01"),
                part: partitionVal
            });
        }
    }
    assert.commandWorked(coll.insert(testDocs));
    let result = coll.aggregate([
        {$project: {_id: 0}},
        {
            $densify: {
                field: "val",
                range: {
                    step: 2,
                    unit: "month",
                    bounds: [new ISODate("2021-05-01"), new ISODate("2021-09-01")]
                },
                partitionByFields: ["part"]
            }
        },
        {$sort: {val: 1, part: 1}}
    ]);
    const resultArray = result.toArray();
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
}
// Test $densify with partitions and explicit bounds that are inside the collection's total range of
// values for the field we are densifying.
function testPartitionsWithBoundsInsideFullRange() {
    coll.drop();
    let collection = [
        {_id: 0, val: 0, part: 1},      {_id: 1, val: 20, part: 2},
        {_id: 2, val: -5, part: 1},     {_id: 3, val: 50, part: 2},
        {_id: 4, val: 106, part: 1},    {_id: 5, val: -50, part: 2},
        {_id: 6, val: 100, part: 1},    {_id: 7, val: -75, part: 2},
        {_id: 8, val: 45, part: 1},     {_id: 9, val: -28, part: 2},
        {_id: 10, val: 67, part: 1},    {_id: 11, val: -19, part: 2},
        {_id: 12, val: -125, part: 1},  {_id: 13, val: -500, part: 2},
        {_id: 14, val: 600, part: 1},   {_id: 15, val: 1000, part: 2},
        {_id: 16, val: -1000, part: 1}, {_id: 17, val: 1400, part: 2},
        {_id: 18, val: 3000, part: 1},  {_id: 19, val: -1900, part: 2},
        {_id: 20, val: -2995, part: 1},
    ];
    assert.commandWorked(coll.insert(collection));
    // Total range is [-2995, 3000] so [-399, -19] is within that.
    let pipeline = [
        {
            $densify:
                {field: "val", range: {step: 17, bounds: [-399, -19]}, partitionByFields: ["part"]}
        },
        {$sort: {part: 1, val: 1}}
    ];
    let result = coll.aggregate(pipeline).toArray();
    assert(anyEq(result.length, 67));
}
// Test negative numbers.
function fullTestFour() {
    coll.drop();
    let testDocs = [];
    let testExpected = [];
    // Add an initial document.
    testDocs.push({val: -10, part: 0});
    testExpected.push({val: -10, part: 0});
    testDocs.push({val: 10, part: 0});
    testExpected.push({val: 10, part: 0});
    testDocs.push({val: -2, part: 1});
    testExpected.push({val: -2, part: 1});
    testExpected.push({val: -10, part: 1});
    for (let densifyVal = -7; densifyVal < 10; densifyVal += 3) {
        testExpected.push({val: densifyVal, part: 0});
        testExpected.push({val: densifyVal, part: 1});
    }
    assert.commandWorked(coll.insert(testDocs));
    let result = coll.aggregate([
        {$project: {_id: 0}},
        {$densify: {field: "val", range: {step: 3, bounds: "full"}, partitionByFields: ["part"]}}
    ]);
    const resultArray = result.toArray();
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
}

// Test a single document collection.
function singleDocumentTest() {
    coll.drop();
    let testDocs = [{val: 1}];
    let testExpected = testDocs;
    assert.commandWorked(coll.insert(testDocs));
    let result = coll.aggregate([
        {$project: {_id: 0}},
        {$densify: {field: "val", range: {step: 1, bounds: "full"}, partitionByFields: ["part"]}},
    ]);
    let resultArray = result.toArray();
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
    result = coll.aggregate([
        {$project: {_id: 0}},
        {
            $densify:
                {field: "val", range: {step: 1, bounds: "partition"}, partitionByFields: ["part"]}
        },
    ]);
    resultArray = result.toArray();
    assert(arrayEq(resultArray, testExpected), buildErrorString(resultArray, testExpected));
}

function testDottedField() {
    coll.drop();
    const input = [
        {
            "_id": 0,
            "metadata": {"sensorId": 5578, "type": "temperature"},
            "timestamp": ISODate("2021-05-18T00:00:00.000Z"),
            "temp": 12
        },
        {
            "_id": 1,
            "metadata": {"sensorId": 5578, "type": "temperature"},
            "timestamp": ISODate("2021-05-18T02:00:00.000Z"),
            "temp": 14
        }
    ];
    const pipeline = [{
        $densify: {
            field: "timestamp",
            // Dots are interpreted as path separators.
            partitionByFields: ["metadata.sensorId"],
            range: {step: 1, unit: "hour", bounds: "full"}
        }
    }];
    const expectedOutput = [
        {
            "_id": 0,
            "metadata": {"sensorId": 5578, "type": "temperature"},
            "timestamp": ISODate("2021-05-18T00:00:00Z"),
            "temp": 12
        },
        {
            // Because dotted fields are interpreted as paths, when we write to 'metadata.sensorId'
            // it should create a nested document.
            "metadata": {"sensorId": 5578},
            "timestamp": ISODate("2021-05-18T01:00:00Z"),
        },
        {
            "_id": 1,
            "metadata": {"sensorId": 5578, "type": "temperature"},
            "timestamp": ISODate("2021-05-18T02:00:00Z"),
            "temp": 14
        }
    ];

    assert.commandWorked(coll.insert(input));
    const actualOutput = coll.aggregate(pipeline).toArray();

    assert(arrayEq(actualOutput, expectedOutput), {actualOutput, expectedOutput});
}

function testArrayTraversalDisallowed() {
    const pipeline = [{
        $densify: {
            field: "timestamp",
            partitionByFields: ["metadata.sensorId"],
        }
    }];

    let input = [
        {
            "_id": 0,
            // In this case, the dot in 'metadata.sensorId' traverses through an array, because
            // the whole 'metadata' subdocument is an array.
            "metadata": [{"sensorId": 5578, "type": "temperature"}],
            "timestamp": ISODate("2021-05-18T00:00:00.000Z"),
            "temp": 12
        },
    ];
    coll.drop();
    assert.commandWorked(coll.insert(input));
    assert.throws(() => coll.aggregate(pipeline));

    input = [
        {
            "_id": 0,
            // In this case, the dot does not traverse an array. But the path evaluates to an array,
            // so this is also an error.
            "metadata": {"sensorId": [5578], "type": "temperature"},
            "timestamp": ISODate("2021-05-18T00:00:00.000Z"),
            "temp": 12
        },
    ];
    coll.drop();
    assert.commandWorked(coll.insert(input));
    assert.throws(() => coll.aggregate(pipeline));
}

testOne();
testTwo();
testThree();
testFour();
testFive();
fullTestOne();
testFive();
fullTestOne(3);
testFive(3);
fullTestTwo();
fullTestThree();
rangeTestOne();
rangeTestTwo();
rangeTestThree();
testPartitionsWithBoundsInsideFullRange();
fullTestFour();

testOneDates();
fullTestTwoDates();
rangeTestTwoDates();

singleDocumentTest();

testDottedField();
testArrayTraversalDisallowed();
})();
