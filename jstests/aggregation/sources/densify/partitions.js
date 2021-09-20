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

const featureEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagDensify: 1}))
        .featureFlagDensify.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the densify feature flag is disabled");
    return;
}

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
        {val: 3, partition: 0},
        {val: 3, partition: 1}
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
    for (let densifyVal = 4; densifyVal <= 8; densifyVal += 2) {
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
fullTestFour();
})();
