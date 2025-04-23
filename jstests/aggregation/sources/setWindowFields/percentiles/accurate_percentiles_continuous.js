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
    assertResultCloseToVal,
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

function continuousPercentile(p) {
    const rank = p * (n - 1);
    const rank_ceil = Math.ceil(rank);
    const rank_floor = Math.floor(rank);
    if (rank_ceil == rank && rank == rank_floor) {
        return docsOrderedByPrice[rank].price;
    } else {
        const linearInterpolate = (rank_ceil - rank) * docsOrderedByPrice[rank_floor].price +
            (rank - rank_floor) * docsOrderedByPrice[rank_ceil].price;
        return linearInterpolate;
    }
}

const paramName = "internalQueryPercentileExprSelectToSortThreshold";
const origParamValue = assert.commandWorked(db.adminCommand(
    {getParameter: 1, internalQueryPercentileExprSelectToSortThreshold: 1}))[paramName];

assert.gte(origParamValue, 0);
let paramValues = [1, 2, origParamValue];

for (var paramValue of paramValues) {
    // The following tests run $percentile for window functions using the continuous method.

    // Run the suite of partition and bounds tests against the $percentile function. Will run
    // tests with removable and non-removable windows.
    testAccumAgainstGroup(
        coll, "$percentile", [null, null], {p: [0.1, 0.6], input: "$price", method: "continuous"});
    testAccumAgainstGroup(coll, "$median", null, {input: "$price", method: "continuous"});

    // Test that $median and $percentile return null for windows which do not contain numeric
    // values.
    let results =
        runSetWindowStage(coll,
                          {$percentile: {p: [0.1, 0.6], input: "$str", method: "continuous"}},
                          {$median: {input: "$str", method: "continuous"}});
    assertResultEqToVal({resultArray: results, percentile: [null, null], median: null});

    results =
        runSetWindowStage(coll,
                          {$percentile: {p: [0.01, 0.99], input: "$price", method: "continuous"}},
                          {$median: {input: "$price", method: "continuous"}});
    assertResultEqToVal({
        resultArray: results,
        percentile: [continuousPercentile(0.01), continuousPercentile(0.99)],
        median: continuousPercentile(0.5)
    });

    // Test that an unbounded window calculates $percentile and $median correctly an continuous
    // method.
    results =
        runSetWindowStage(coll,
                          {$percentile: {p: [0.01, 0.99], input: "$price", method: "continuous"}},
                          {$median: {input: "$price", method: "continuous"}});
    assertResultEqToVal({
        resultArray: results,
        percentile: [continuousPercentile(0.01), continuousPercentile(0.99)],
        median: continuousPercentile(0.5)
    });

    // Test that an expression can be used for 'input'.
    results = runSetWindowStage(
        coll,
        {$percentile: {p: [0.01, 0.99], input: {$add: [42, "$price"]}, method: "continuous"}},
        {$median: {input: {$add: [42, "$price"]}, method: "continuous"}});
    // TODO SERVER-91956: mongod returns 443.90000000000003 for p=0.01 due to precision. The
    // correct answer is 443.9. When we have better precision, change this to use
    // assertResultEqToVal.
    assertResultCloseToVal({
        resultArray: results,
        percentile: [42 + continuousPercentile(0.01), 42 + continuousPercentile(0.99)],
        median: 42 + continuousPercentile(0.5)
    });

    // Test that a variable can be used for 'p'.
    results = runSetWindowStage(coll,
                                {$percentile: {p: "$$ps", input: "$price", method: "continuous"}},
                                {$median: {input: "$price", method: "continuous"}},
                                {ps: [0.01, 0.99]});
    assertResultEqToVal({
        resultArray: results,
        percentile: [continuousPercentile(0.01), continuousPercentile(0.99)],
        median: continuousPercentile(0.5)
    });

    // Test that a removable window calculates $percentile and $median correctly using a
    // continuous method.
    results = runSetWindowStage(
        coll,
        {
            $percentile: {p: [0.9], input: "$price", method: "continuous"},
            window: {documents: [-1, 0]}
        },
        {$median: {input: "$price", method: "continuous"}, window: {documents: [-1, 0]}});
    // With a window of size 2 the percentile should always be linear interpolated between the
    // two prices.
    for (let index = 0; index < results.length; index++) {
        let prevIndex = Math.max(0, index - 1);  // get the document before the current
        let maxVal = Math.max(origDocs[prevIndex].price, origDocs[index].price);
        let minVal = Math.min(origDocs[prevIndex].price, origDocs[index].price);
        // rank is just p since n = 2, rank = p * (n - 1)
        let percentile = (1 - 0.9) * minVal + (0.9 - 0) * maxVal;
        let median = (1 - 0.5) * minVal + (0.5 - 0) * maxVal;
        assert.eq(percentile, results[index].runningPercentile);
        assert.eq(median, results[index].runningMedian);
    }
}
