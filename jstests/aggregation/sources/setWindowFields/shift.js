/**
 * Test that $shift works as a window function.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // documentEq

const coll = db[jsTestName()];
coll.drop();

const nDocs = 10;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({
        one: i,
        partition: i % 2,
        partitionSeq: Math.trunc(i / 2),
    }));
}
const lastDoc = nDocs - 1;
const lastDocInPartition = nDocs / 2 - 1;

const origDocs = coll.find().sort({_id: 1});
function verifyResults(results, valueFunction) {
    for (let i = 0; i < results.length; i++) {
        // Use Object.assign to make a copy instead of pass a reference.
        const correctDoc = valueFunction(i, Object.assign({}, origDocs[i]));
        assert(documentEq(correctDoc, results[i]),
               "Got: " + tojson(results[i]) + "\nExpected: " + tojson(correctDoc) +
                   "\n at position " + i + "\n");
    }
}

// Run an unpartitioned shift query using the specified offset and default expression.
function runShiftQuery(shiftBy, defaultVal) {
    return coll
        .aggregate([
            {
                $setWindowFields: {
                    sortBy: {one: 1},
                    output: {a: {$shift: {by: shiftBy, output: "$one", default: defaultVal}}}
                }
            },
            {$sort: {_id: 1}}
        ])
        .toArray();
}

// Run an unpartitioned shift query using the specified offset and the default default expression.
function runShiftQueryWithoutDefault(shiftBy) {
    return coll
        .aggregate([
            {
                $setWindowFields:
                    {sortBy: {one: 1}, output: {a: {$shift: {by: shiftBy, output: "$one"}}}}
            },
            {$sort: {_id: 1}}
        ])
        .toArray();
}

// Test left shift with default.
let result = runShiftQuery(-1, -10);
verifyResults(result, function(num, baseObj) {
    if (baseObj.one == 0)
        baseObj.a = -10;
    else
        baseObj.a = baseObj.one - 1;
    return baseObj;
});

// Test left shift without default.
result = runShiftQueryWithoutDefault(-1);
verifyResults(result, function(num, baseObj) {
    if (baseObj.one == 0)
        baseObj.a = null;
    else
        baseObj.a = baseObj.one - 1;
    return baseObj;
});

// Test 0 shift with default.
result = runShiftQuery(0);
verifyResults(result, function(num, baseObj) {
    baseObj.a = baseObj.one;
    return baseObj;
});

// Test 0 shift without default.
result = runShiftQueryWithoutDefault(0);
verifyResults(result, function(num, baseObj) {
    baseObj.a = baseObj.one;
    return baseObj;
});

// Test right shift with default.
result = runShiftQuery(1, -10);
verifyResults(result, function(num, baseObj) {
    if (baseObj.one == lastDoc)
        baseObj.a = -10;
    else
        baseObj.a = baseObj.one + 1;
    return baseObj;
});

// Test right shift without default.
result = runShiftQueryWithoutDefault(1);
verifyResults(result, function(num, baseObj) {
    if (baseObj.one == lastDoc)
        baseObj.a = null;
    else
        baseObj.a = baseObj.one + 1;
    return baseObj;
});

// Run an unpartitioned shift query using the specified offset with descending order.
function runShiftQueryDescending(shiftBy) {
    return coll
        .aggregate([
            {
                $setWindowFields:
                    {sortBy: {one: -1}, output: {a: {$shift: {by: shiftBy, output: "$one"}}}}
            },
            {$sort: {_id: 1}}
        ])
        .toArray();
}

// Test right shift with descending sort.
result = runShiftQueryDescending(1);
verifyResults(result, function(num, baseObj) {
    if (baseObj.one == 0)
        baseObj.a = null;
    else
        baseObj.a = baseObj.one - 1;
    return baseObj;
});

// Test 0 shift with descending sort.
result = runShiftQueryDescending(0);
verifyResults(result, function(num, baseObj) {
    baseObj.a = baseObj.one;
    return baseObj;
});

// Test left shift with descending sort.
result = runShiftQueryDescending(-1);
verifyResults(result, function(num, baseObj) {
    if (baseObj.one == lastDoc)
        baseObj.a = null;
    else
        baseObj.a = baseObj.one + 1;
    return baseObj;
});

// Run a shift query partitioned over "$partition" using the specified shift and default
// default expression.
//
// Partitioning is odd/even.
function runPartitionedShiftQuery(shiftBy) {
    return coll
        .aggregate([
            {
                $setWindowFields: {
                    partitionBy: "$partition",
                    sortBy: {one: 1},
                    output: {a: {$shift: {by: shiftBy, output: "$one"}}}
                }
            },
            {$sort: {_id: 1}}
        ])
        .toArray();
}

// Test partitioned left shift.
result = runPartitionedShiftQuery(-1);
verifyResults(result, function(num, baseObj) {
    if (baseObj.partitionSeq == 0)
        baseObj.a = null;
    else
        // partitioning is even/odd.
        baseObj.a = baseObj.one - 2;
    return baseObj;
});

// Test partitioned right shift.
result = runPartitionedShiftQuery(1);
verifyResults(result, function(num, baseObj) {
    if (baseObj.partitionSeq == lastDocInPartition)
        baseObj.a = null;
    else
        // partitioning is even/odd.
        baseObj.a = baseObj.one + 2;
    return baseObj;
});

// Test partitioned 0 shift.
result = runPartitionedShiftQuery(0);
verifyResults(result, function(num, baseObj) {
    baseObj.a = baseObj.one;
    return baseObj;
});

// Run a shift query partitioned over "$partition" using the specified shift and default
// default expression with a descending sort.
//
// Partitioning is odd/even.
function runPartitionedShiftQueryDescending(shiftBy) {
    return coll
        .aggregate([
            {
                $setWindowFields: {
                    partitionBy: "$partition",
                    sortBy: {one: -1},
                    output: {a: {$shift: {by: shiftBy, output: "$one"}}}
                }
            },
            {$sort: {_id: 1}}
        ])
        .toArray();
}

// Test partitioned left shift with descending sort.
result = runPartitionedShiftQueryDescending(-1);
verifyResults(result, function(num, baseObj) {
    if (baseObj.partitionSeq == lastDocInPartition)
        baseObj.a = null;
    else
        // partitioning is even/odd.
        baseObj.a = baseObj.one + 2;
    return baseObj;
});

// Test partitioned right shift with descending sort.
result = runPartitionedShiftQueryDescending(1);
verifyResults(result, function(num, baseObj) {
    if (baseObj.partitionSeq == 0)
        baseObj.a = null;
    else
        // partitioning is even/odd.
        baseObj.a = baseObj.one - 2;
    return baseObj;
});

// Test partitioned 0 shift with descending sort.
result = runPartitionedShiftQuery(0);
verifyResults(result, function(num, baseObj) {
    baseObj.a = baseObj.one;
    return baseObj;
});

/* Parsing tests */

// "by" is required.
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$setWindowFields: {sortBy: {one: 1}, output: {a: {$shift: {output: "$one"}}}}}],
    cursor: {}
}),
                             ErrorCodes.FailedToParse);

// Can't accept a string for "by".
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline:
        [{$setWindowFields: {sortBy: {one: 1}, output: {a: {$shift: {by: "1", output: "$one"}}}}}],
    cursor: {}
}),
                             ErrorCodes.FailedToParse);

// Can't accept an expression for "by".
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $setWindowFields:
            {sortBy: {one: 1}, output: {a: {$shift: {by: {$sum: [1, 1]}, output: "$one"}}}}
    }],
    cursor: {}
}),
                             ErrorCodes.FailedToParse);

// Can't accept a float for "by".
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline:
        [{$setWindowFields: {sortBy: {one: 1}, output: {a: {$shift: {by: 1.1, output: "$one"}}}}}],
    cursor: {}
}),
                             ErrorCodes.FailedToParse);

// Can't accept a float for "by" ... unless it converts to int without loss of precision.
assert.commandWorked(coll.runCommand({
    aggregate: coll.getName(),
    pipeline:
        [{$setWindowFields: {sortBy: {one: 1}, output: {a: {$shift: {by: 1.0, output: "$one"}}}}}],
    cursor: {}
}));

// "output" is required.
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$setWindowFields: {sortBy: {one: 1}, output: {a: {$shift: {by: 1}}}}}],
    cursor: {}
}),
                             ErrorCodes.FailedToParse);

// "default" must evaluate to a constant.
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $setWindowFields:
            {sortBy: {one: 1}, output: {a: {$shift: {by: 1, output: "$one", default: "$one"}}}}
    }],
    cursor: {}
}),
                             ErrorCodes.FailedToParse);

// "default" may be an arbitrary expression as long as it evaluates to a constant.
assert.commandWorked(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $setWindowFields: {
            sortBy: {one: 1},
            output: {a: {$shift: {by: 1, output: "$one", default: {$add: [1, 1]}}}}
        }
    }],
    cursor: {}
}));

// "sortBy" is required for $shift.
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$setWindowFields: {output: {a: {$shift: {by: 1, output: "$one"}}}}}],
    cursor: {}
}),
                             ErrorCodes.FailedToParse);
})();
