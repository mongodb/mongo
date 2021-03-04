/**
 * Test that $avg works as a window function.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");

const featureEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagWindowFunctions: 1}))
        .featureFlagWindowFunctions.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the window function feature flag is disabled");
    return;
}

const coll = db[jsTestName()];
coll.drop();

// Create a collection of tickers and prices.
const nDocsPerTicker = 10;
seedWithTickerData(coll, nDocsPerTicker);

// Run the suite of partition and bounds tests against the $avg function.
testAccumAgainstGroup(coll, "$avg", nDocsPerTicker);

// Test a combination of two different runnning averages.
let results =
    coll.aggregate([
            {
                $setWindowFields: {
                    sortBy: {_id: 1},
                    partitionBy: "$ticker",
                    output: {
                        runningAvg: {$avg: "$price", window: {documents: ["unbounded", "current"]}},
                        runningAvgLead: {$avg: "$price", window: {documents: ["unbounded", 3]}},
                    }
                },
            },
        ])
        .toArray();
for (let index = 0; index < results.length; index++) {
    // First compute the 'runningAvg' with 0 as the upper bound.
    let groupRes = computeAsGroup({
        coll: coll,
        partitionKey: {ticker: results[index].ticker},
        accum: "$avg",
        lower: 0,
        upper: results[index].partIndex
    });
    assert.eq(groupRes, results[index].runningAvg);

    // Now compute the 'runningAvgLead' with 3 as the upper bound.
    groupRes = computeAsGroup({
        coll: coll,
        partitionKey: {ticker: results[index].ticker},
        accum: "$avg",
        lower: 0,
        upper: results[index].partIndex + 3
    });
    assert.eq(groupRes, results[index].runningAvgLead);
}
})();
