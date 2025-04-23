/**
 * Test that $percentile and $median work as window functions w/ approximate methods.
 *  @tags: [
 *   requires_fcv_81,
 *   # $setParameter is not compatible with signed security tokens
 *   simulate_mongoq_incompatible,
 *   featureFlagAccuratePercentiles,
 * ]
 */

import {
    seedWithTickerData,
    testAccumAgainstGroup
} from "jstests/aggregation/extras/window_function_helpers.js";
import {
    assertResultEqToVal,
    runSetWindowStage
} from "jstests/aggregation/sources/setWindowFields/percentiles/percentile_util.js";

const coll = db[jsTestName()];
coll.drop();

// create a collection with documents.
const nDocsPerTicker = 10;
seedWithTickerData(coll, nDocsPerTicker);

const origDocs = coll.find().sort({_id: 1}).toArray();

const docsOrderedByPrice = coll.find().sort({"price": 1}).toArray();
const maxDoc = docsOrderedByPrice[docsOrderedByPrice.length - 1];
const minDoc = docsOrderedByPrice[0];
const medianDoc = docsOrderedByPrice[Math.floor(docsOrderedByPrice.length / 2) - 1];
const n = docsOrderedByPrice.length;

const paramName = "internalQueryPercentileExprSelectToSortThreshold";
const origParamValue = assert.commandWorked(db.adminCommand(
    {getParameter: 1, internalQueryPercentileExprSelectToSortThreshold: 1}))[paramName];

assert.gte(origParamValue, 0);
let paramValues = [1, 2, origParamValue];

for (var paramValue of paramValues) {
    // Set the percentile sorting threshold to test pre-sorting before calculating percentiles
    // as well sorting on each percentile calculation.
    db.adminCommand(
        {setParameter: 1, internalQueryPercentileExprSelectToSortThreshold: paramValue});

    jsTestLog("internalQueryPercentileExprSelectToSortThreshold value is now " + paramValue);

    // The following tests run $percentile for window functions using the discrete method.

    // Run the suite of partition and bounds tests against the $percentile function. Will run
    // tests with removable and non-removable windows.
    testAccumAgainstGroup(
        coll, "$percentile", [null, null], {p: [0.1, 0.6], input: "$price", method: "discrete"});
    testAccumAgainstGroup(coll, "$median", null, {input: "$price", method: "discrete"});

    // Test that $median and $percentile return null for windows which do not contain numeric
    // values.
    let results =
        runSetWindowStage(coll,
                          {$percentile: {p: [0.1, 0.6], input: "$str", method: "discrete"}},
                          {$median: {input: "$str", method: "discrete"}});
    assertResultEqToVal({resultArray: results, percentile: [null, null], median: null});

    // Test that an unbounded window calculates $percentile and $median correctly with discrete
    // method.
    results =
        runSetWindowStage(coll,
                          {$percentile: {p: [0.01, 0.99], input: "$price", method: "discrete"}},
                          {$median: {input: "$price", method: "discrete"}});
    // Since our percentiles are 0.01 and 0.99 and our collection is small, we will always
    // return the minimum and maximum value in the collection.
    assertResultEqToVal(
        {resultArray: results, percentile: [minDoc.price, maxDoc.price], median: medianDoc.price});

    // Test that an expression can be used for 'input'.
    results = runSetWindowStage(
        coll,
        {$percentile: {p: [0.01, 0.99], input: {$add: [42, "$price"]}, method: "discrete"}},
        {$median: {input: {$add: [42, "$price"]}, method: "discrete"}});
    // Since our percentiles are 0.01 and 0.99 and our collection is small, we will always
    // return the minimum and maximum value in the collection.
    assertResultEqToVal({
        resultArray: results,
        percentile: [42 + minDoc.price, 42 + maxDoc.price],
        median: 42 + medianDoc.price
    });

    // Test that a variable can be used for 'p'.
    results = runSetWindowStage(coll,
                                {$percentile: {p: "$$ps", input: "$price", method: "discrete"}},
                                {$median: {input: "$price", method: "discrete"}},
                                {ps: [0.01, 0.99]});
    // Since our percentiles are 0.01 and 0.99 and our collection is small, we will always
    // return the minimum and maximum value in the collection.
    assertResultEqToVal(
        {resultArray: results, percentile: [minDoc.price, maxDoc.price], median: medianDoc.price});

    // Test that a removable window calculates $percentile and $median correctly using a
    // discrete method.
    results = runSetWindowStage(
        coll,
        {
            $percentile: {p: [0.9], input: "$price", method: "discrete"},
            window: {documents: [-1, 0]}
        },
        {$median: {input: "$price", method: "discrete"}, window: {documents: [-1, 0]}});
    // With a window of size 2 the 0.9 percentile should always be the maximum document
    // in our window, and the median will be the other document in the window.
    for (let index = 0; index < results.length; index++) {
        let prevIndex = Math.max(0, index - 1);  // get the document before the current
        let maxVal = Math.max(origDocs[prevIndex].price, origDocs[index].price);
        let minVal = Math.min(origDocs[prevIndex].price, origDocs[index].price);

        assert.eq(maxVal, results[index].runningPercentile, results[index]);
        assert.eq(minVal, results[index].runningMedian, results[index]);
    }
}
