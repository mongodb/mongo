/**
 * Basic tests for the $minN/$maxN accumulators.
 */
(function() {
"use strict";

const coll = db[jsTestName()];
coll.drop();

// Basic correctness tests.
let docs = [];
const n = 4;

const largestInt =
    NumberDecimal("9223372036854775807");  // This is max int64 which is supported as N.
const largestIntPlus1 = NumberDecimal("9223372036854775808");  // Adding 1 puts it over the edge.

// Some big number that will allow us to test big without running this testcase into next decade.
const bigN = 10000;

const states = [
    {state: 'CA', sales: 10},
    {state: 'NY', sales: 7},
    {state: 'TX', sales: 4},
    {state: 'WY', sales: bigN}
];
let expectedMinNResults = [];
let expectedMaxNResults = [];
let expectedLargestIntMinNResults = [];
let expectedLargestIntMaxNResults = [];
let expectedBigNMinNResults = [];
let expectedBigNMaxNResults = [];

for (const stateDoc of states) {
    const state = stateDoc['state'];
    const sales = stateDoc['sales'];
    let minArr = [];
    let maxArr = [];
    let minArrForLargestInt = [];
    let maxArrForLargestInt = [];
    let minArrForBigN = [];
    let maxArrForBigN = [];
    for (let i = 1; i <= sales; ++i) {
        const amount = i * 100;
        docs.push({state: state, sales: amount});

        // Record the lowest/highest 'n' values.
        if (i < n + 1) {
            minArr.push(amount);
        }
        if (sales - n < i) {
            maxArr.push(amount);
        }

        // Always push into the arrays for the largestInt
        minArrForLargestInt.push(amount);
        maxArrForLargestInt.push(amount);

        // For the bigInt array, record the lowest/highest 'n-1' values.
        // This is because we will do a bigN-1 in the actual query to properly validate the result.
        if (i < bigN) {
            minArrForBigN.push(amount);
        }
        if (sales - bigN < i - 1) {
            maxArrForBigN.push(amount);
        }
    }
    expectedMinNResults.push({_id: state, sales: minArr});
    expectedLargestIntMinNResults.push({_id: state, sales: minArrForLargestInt});
    expectedBigNMinNResults.push({_id: state, sales: minArrForBigN});

    // Reverse 'maxArr' results since $maxN outputs results in descending order.
    expectedMaxNResults.push({_id: state, sales: maxArr.reverse()});
    expectedLargestIntMaxNResults.push({_id: state, sales: maxArrForLargestInt.reverse()});
    expectedBigNMaxNResults.push({_id: state, sales: maxArrForBigN.reverse()});
}

assert.commandWorked(coll.insert(docs));

// Run a minN/maxN query and compare it against the expectedResult.
// Note that the output documents are sorted by '_id' so that we can compare actual groups against
// expected groups (we cannot perform unordered comparison because order matters for $minN/$maxN).
function runAndCompareMinMaxN(nFunction, n, expectedResults) {
    const actualResults =
        coll.aggregate([
                {$group: {_id: '$state', sales: {[nFunction]: {input: '$sales', n: n}}}},
                {$sort: {_id: 1}}
            ])
            .toArray();
    assert.eq(expectedResults, actualResults);

    // Basic correctness test for $minN/$maxN used in $bucketAuto. Though $bucketAuto uses
    // accumulators in the same way that $group does, the test below verifies that everything
    // works properly with serialization and reporting results. Note that the $project allows us
    // to compare the $bucketAuto results to the expected $group results (because there are more
    // buckets than groups, it will always be the case that the min value of each bucket
    // corresponds to the group key).
    let actualBucketAutoResults =
        coll.aggregate([
                {
                    $bucketAuto: {
                        groupBy: '$state',
                        buckets: 10 * 1000,
                        output: {sales: {[nFunction]: {input: '$sales', n: n}}}
                    }
                },
                {$project: {_id: "$_id.min", sales: 1}},
                {$sort: {_id: 1}},
            ])
            .toArray();

    // Using a computed projection will put the fields out of order. As such, we re-order them
    // below.
    for (let i = 0; i < actualBucketAutoResults.length; ++i) {
        const currentDoc = actualBucketAutoResults[i];
        actualBucketAutoResults[i] = {_id: currentDoc._id, sales: currentDoc.sales};
    }
    assert.eq(expectedResults, actualBucketAutoResults);
}

// Test for regular N.
runAndCompareMinMaxN("$minN", n, expectedMinNResults);
runAndCompareMinMaxN("$maxN", n, expectedMaxNResults);

// Verify N can be as large as the largest integer.
runAndCompareMinMaxN("$minN", largestInt, expectedLargestIntMinNResults);
runAndCompareMinMaxN("$maxN", largestInt, expectedLargestIntMaxNResults);

// Verify a large value for N, but still do not return all of all values.
runAndCompareMinMaxN("$minN", bigN - 1, expectedBigNMinNResults);
runAndCompareMinMaxN("$maxN", bigN - 1, expectedBigNMaxNResults);

// Verify that we can dynamically compute 'n' based on the group key for $group.
const groupKeyNExpr = {
    $cond: {if: {$eq: ['$st', 'CA']}, then: 10, else: 4}
};
const dynamicMinNResults =
    coll.aggregate([{
            $group: {_id: {'st': '$state'}, minSales: {$minN: {input: '$sales', n: groupKeyNExpr}}}
        }])
        .toArray();

// Verify that the 'CA' group has 10 results, while all others have only 4.
for (const result of dynamicMinNResults) {
    assert(result.hasOwnProperty('_id'), tojson(result));
    const groupKey = result['_id'];
    assert(groupKey.hasOwnProperty('st'), tojson(groupKey));
    const state = groupKey['st'];
    assert(result.hasOwnProperty('minSales'), tojson(result));
    const salesArray = result['minSales'];
    if (state === 'CA') {
        assert.eq(salesArray.length, 10, tojson(salesArray));
    } else {
        assert.eq(salesArray.length, 4, tojson(salesArray));
    }
}

// Error cases

// Cannot reference the group key in $minN when using $bucketAuto.
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{
        $bucketAuto: {
            groupBy: "$state",
            buckets: 2,
            output: {minSales: {$minN: {input: '$sales', n: groupKeyNExpr}}}
        }
    }],
    cursor: {}
}),
                             4544714);

// Reject non-integral/negative values of n.
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline:
        [{$group: {_id: {'st': '$state'}, minSales: {$minN: {input: '$sales', n: 'string'}}}}],
    cursor: {}
}),
                             5787902);

assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{$group: {_id: {'st': '$state'}, minSales: {$minN: {input: '$sales', n: 3.2}}}}],
    cursor: {}
}),
                             5787903);

assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{$group: {_id: {'st': '$state'}, minSales: {$minN: {input: '$sales', n: -1}}}}],
    cursor: {}
}),
                             5787908);

assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{$group: {_id: {'st': '$state'}, minSales: {$minN: {input: '$sales', n: 0}}}}],
    cursor: {}
}),
                             5787908);

assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [
        {$group: {_id: {'st': '$state'}, minSales: {$minN: {input: '$sales', n: largestIntPlus1}}}}
    ],
    cursor: {}
}),
                             5787903);

// Reject invalid specifications.

// Missing arguments.
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{$group: {_id: {'st': '$state'}, minSales: {$minN: {input: '$sales'}}}}],
    cursor: {}
}),
                             5787906);

assert.commandFailedWithCode(
    coll.runCommand(
        "aggregate",
        {pipeline: [{$group: {_id: {'st': '$state'}, minSales: {$minN: {n: 2}}}}], cursor: {}}),
    5787907);

// Extra field.
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{
        $group: {
            _id: {'st': '$state'},
            minSales: {$minN: {input: '$sales', n: 2, randomField: "randomArg"}}
        }
    }],
    cursor: {}
}),
                             5787901);
})();
