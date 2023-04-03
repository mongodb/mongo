/**
 * Test that $percentile and $median work as window functions.
 *  @tags: [
 *   requires_fcv_70,
 *   featureFlagApproxPercentiles
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");

const coll = db[jsTestName()];
coll.drop();

// create a collection with documents.
const nDocsPerTicker = 10;
seedWithTickerData(coll, nDocsPerTicker);

const origDocs = coll.find().sort({_id: 1}).toArray();

const docsOrderedByPrice = coll.find().sort({"price": -1}).toArray();
const maxDoc = docsOrderedByPrice[0];
const minDoc = docsOrderedByPrice[docsOrderedByPrice.length - 1];
const medianDoc = docsOrderedByPrice[Math.floor(docsOrderedByPrice.length / 2) - 1];

// Run the suite of partition and bounds tests against the $percentile function. Will run tests with
// removable and non-removable windows.
testAccumAgainstGroup(
    coll, "$percentile", [null, null], {p: [0.1, 0.6], input: "$price", method: "approximate"});
testAccumAgainstGroup(coll, "$median", null, {input: "$price", method: "approximate"});

function runSetWindowStage(percentileSpec, medianSpec) {
    return coll
        .aggregate([
            {$addFields: {str: "hiya"}},
            {
                $setWindowFields: {
                    sortBy: {_id: 1},
                    output: {
                        runningPercentile: percentileSpec,
                        runningMedian: medianSpec,
                    }
                }
            }
        ])
        .toArray();
}

function assertResultEqToVal({resultArray: results, percentile: pVal, median: mVal}) {
    for (let index = 0; index < results.length; index++) {
        assert.eq(pVal, results[index].runningPercentile);
        assert.eq(mVal, results[index].runningMedian);
    }
}

// Test that $median and $percentile return null for windows which do not contain numeric values.
let results =
    runSetWindowStage({$percentile: {p: [0.1, 0.6], input: "$str", method: "approximate"}},
                      {$median: {input: "$str", method: "approximate"}});
assertResultEqToVal({resultArray: results, percentile: [null, null], median: null});

// Test that an unbounded window calculates $percentile and $median correctly an approximate method.
results =
    runSetWindowStage({$percentile: {p: [0.01, 0.99], input: "$price", method: "approximate"}},
                      {$median: {input: "$price", method: "approximate"}});
// Since our percentiles are 0.01 and 0.99 and our collection is small, we will always return the
// minimum and maximum value in the collection.
assertResultEqToVal(
    {resultArray: results, percentile: [minDoc.price, maxDoc.price], median: medianDoc.price});

// Test that a removable window calculates $percentile and $median correctly using an approximate
// method.
results = runSetWindowStage(
    {$percentile: {p: [0.9], input: "$price", method: "approximate"}, window: {documents: [-1, 0]}},
    {$median: {input: "$price", method: "approximate"}, window: {documents: [-1, 0]}});
// With a window of size 2 the 0.9 percentile should always be the maximum document
// in our window, and the median will be the other document in the window.
for (let index = 0; index < results.length; index++) {
    let prevIndex = Math.max(0, index - 1);  // get the document before the current
    let maxVal = Math.max(origDocs[prevIndex].price, origDocs[index].price);
    let minVal = Math.min(origDocs[prevIndex].price, origDocs[index].price);

    assert.eq(maxVal, results[index].runningPercentile, results[index]);
    assert.eq(minVal, results[index].runningMedian, results[index]);
}

function testError(percentileSpec, expectedCode) {
    assert.throwsWithCode(() => coll.aggregate([{
        $setWindowFields: {
            partitionBy: "$ticket",
            sortBy: {ts: 1},
            output: {outputField: percentileSpec},
        }
    }]),
                          expectedCode);
}

// Invalid window specification.
testError({$percentile: {p: [0.1, 0.6], input: "$str", method: "approximate"}, window: [-1, 1]},
          ErrorCodes.FailedToParse);
testError({$median: {input: "$str", method: "approximate"}, window: [-1, 1]},
          ErrorCodes.FailedToParse);
testError({$percentile: {p: [0.6], input: "$str", method: "approximate"}, window: {documents: []}},
          ErrorCodes.FailedToParse);
testError({$median: {input: "$str", method: "approximate"}, window: {documents: []}},
          ErrorCodes.FailedToParse);

// Extra argument in the window function.
testError({$percentile: {p: [0.1, 0.6], input: "$str", method: "approximate"}, extra: "extra"},
          ErrorCodes.FailedToParse);
testError({$median: {input: "$str", method: "approximate"}, extra: "extra"},
          ErrorCodes.FailedToParse);

// Invalid input for the accumulators.
testError({$percentile: "not an object"}, 7429703);
testError({$median: "not an object"}, 7436100);

testError({$percentile: {p: [0.1, 0.6], input: "$str", method: false}}, ErrorCodes.TypeMismatch);
testError({$median: {input: "$str", method: false}}, ErrorCodes.TypeMismatch);

testError({$percentile: {input: "$str", method: "approximate"}},
          40414 /* IDL required field error */);
testError({$median: {p: [0.1, 0.6], input: "$str", method: "approximate"}},
          40415 /* IDL unknown field error */);
})();
