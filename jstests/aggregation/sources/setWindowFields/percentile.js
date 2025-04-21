/**
 * Test that $percentile and $median work as window functions.
 *  @tags: [
 *   requires_fcv_81,
 * ]
 */
import {
    seedWithTickerData,
    testAccumAgainstGroup
} from "jstests/aggregation/extras/window_function_helpers.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

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

function runSetWindowStage(percentileSpec, medianSpec, letSpec) {
    return coll
        .aggregate(
            [
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
            ],
            {let : letSpec})
        .toArray();
}

function assertResultEqToVal({resultArray: results, percentile: pVal, median: mVal}) {
    for (let index = 0; index < results.length; index++) {
        assert.eq(pVal, results[index].runningPercentile);
        assert.eq(mVal, results[index].runningMedian);
    }
}

function assertResultCloseToVal({resultArray: results, percentile: pVal, median: mVal}) {
    for (let index = 0; index < results.length; index++) {
        // TODO SERVER-91956: Under some circumstances, mongod returns slightly wrong answers due to
        // precision. When mongod has better precision, this function can be removed in favor of
        // assertResultEqToVal.
        for (let percentileIndex = 0; percentileIndex < pVal.length; percentileIndex++) {
            assert.close(pVal[percentileIndex], results[index].runningPercentile[percentileIndex]);
        }
        assert.close(mVal, results[index].runningMedian);
    }
}

// The following tests run $percentile for window functions using the approximate method, which uses
// the discrete computation.

// Run the suite of partition and bounds tests against the $percentile function. Will run tests with
// removable and non-removable windows.
testAccumAgainstGroup(
    coll, "$percentile", [null, null], {p: [0.1, 0.6], input: "$price", method: "approximate"});
testAccumAgainstGroup(coll, "$median", null, {input: "$price", method: "approximate"});

// Test that $median and $percentile return null for windows which do not contain numeric values.
let results =
    runSetWindowStage({$percentile: {p: [0.1, 0.6], input: "$str", method: "approximate"}},
                      {$median: {input: "$str", method: "approximate"}});
assertResultEqToVal({resultArray: results, percentile: [null, null], median: null});

// Test that an unbounded window calculates $percentile and $median correctly with approximate
// method.
results =
    runSetWindowStage({$percentile: {p: [0.01, 0.99], input: "$price", method: "approximate"}},
                      {$median: {input: "$price", method: "approximate"}});
// Since our percentiles are 0.01 and 0.99 and our collection is small, we will always return the
// minimum and maximum value in the collection.
assertResultEqToVal(
    {resultArray: results, percentile: [minDoc.price, maxDoc.price], median: medianDoc.price});

// Test that an expression can be used for 'input'.
results = runSetWindowStage(
    {$percentile: {p: [0.01, 0.99], input: {$add: [42, "$price"]}, method: "approximate"}},
    {$median: {input: {$add: [42, "$price"]}, method: "approximate"}});
// Since our percentiles are 0.01 and 0.99 and our collection is small, we will always return the
// minimum and maximum value in the collection.
assertResultEqToVal({
    resultArray: results,
    percentile: [42 + minDoc.price, 42 + maxDoc.price],
    median: 42 + medianDoc.price
});

// Test that a variable can be used for 'p'.
results = runSetWindowStage({$percentile: {p: "$$ps", input: "$price", method: "approximate"}},
                            {$median: {input: "$price", method: "approximate"}},
                            {ps: [0.01, 0.99]});
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

if (FeatureFlagUtil.isPresentAndEnabled(db, "AccuratePercentiles")) {
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
        testAccumAgainstGroup(coll,
                              "$percentile",
                              [null, null],
                              {p: [0.1, 0.6], input: "$price", method: "discrete"});
        testAccumAgainstGroup(coll, "$median", null, {input: "$price", method: "discrete"});

        // Test that $median and $percentile return null for windows which do not contain numeric
        // values.
        results =
            runSetWindowStage({$percentile: {p: [0.1, 0.6], input: "$str", method: "discrete"}},
                              {$median: {input: "$str", method: "discrete"}});
        assertResultEqToVal({resultArray: results, percentile: [null, null], median: null});

        // Test that an unbounded window calculates $percentile and $median correctly with discrete
        // method.
        results =
            runSetWindowStage({$percentile: {p: [0.01, 0.99], input: "$price", method: "discrete"}},
                              {$median: {input: "$price", method: "discrete"}});
        // Since our percentiles are 0.01 and 0.99 and our collection is small, we will always
        // return the minimum and maximum value in the collection.
        assertResultEqToVal({
            resultArray: results,
            percentile: [minDoc.price, maxDoc.price],
            median: medianDoc.price
        });

        // Test that an expression can be used for 'input'.
        results = runSetWindowStage(
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
        results = runSetWindowStage({$percentile: {p: "$$ps", input: "$price", method: "discrete"}},
                                    {$median: {input: "$price", method: "discrete"}},
                                    {ps: [0.01, 0.99]});
        // Since our percentiles are 0.01 and 0.99 and our collection is small, we will always
        // return the minimum and maximum value in the collection.
        assertResultEqToVal({
            resultArray: results,
            percentile: [minDoc.price, maxDoc.price],
            median: medianDoc.price
        });

        // Test that a removable window calculates $percentile and $median correctly using a
        // discrete method.
        results = runSetWindowStage(
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

        // The following tests run $percentile for window functions using the continuous method.

        // Run the suite of partition and bounds tests against the $percentile function. Will run
        // tests with removable and non-removable windows.
        testAccumAgainstGroup(coll,
                              "$percentile",
                              [null, null],
                              {p: [0.1, 0.6], input: "$price", method: "continuous"});
        testAccumAgainstGroup(coll, "$median", null, {input: "$price", method: "continuous"});

        // Test that $median and $percentile return null for windows which do not contain numeric
        // values.
        results =
            runSetWindowStage({$percentile: {p: [0.1, 0.6], input: "$str", method: "continuous"}},
                              {$median: {input: "$str", method: "continuous"}});
        assertResultEqToVal({resultArray: results, percentile: [null, null], median: null});

        results = runSetWindowStage(
            {$percentile: {p: [0.01, 0.99], input: "$price", method: "continuous"}},
            {$median: {input: "$price", method: "continuous"}});
        assertResultEqToVal({
            resultArray: results,
            percentile: [continuousPercentile(0.01), continuousPercentile(0.99)],
            median: continuousPercentile(0.5)
        });

        // Test that an unbounded window calculates $percentile and $median correctly an continuous
        // method.
        results = runSetWindowStage(
            {$percentile: {p: [0.01, 0.99], input: "$price", method: "continuous"}},
            {$median: {input: "$price", method: "continuous"}});
        assertResultEqToVal({
            resultArray: results,
            percentile: [continuousPercentile(0.01), continuousPercentile(0.99)],
            median: continuousPercentile(0.5)
        });

        // Test that an expression can be used for 'input'.
        results = runSetWindowStage(
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
        results =
            runSetWindowStage({$percentile: {p: "$$ps", input: "$price", method: "continuous"}},
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
}

function testError(percentileSpec, expectedCode, letSpec) {
    assert.throwsWithCode(() => coll.aggregate([{
                                                   $setWindowFields: {
                                                       partitionBy: "$ticket",
                                                       sortBy: {ts: 1},
                                                       output: {outputField: percentileSpec},
                                                   }
                                               }],
                                               {let : letSpec}),
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
if (!FeatureFlagUtil.isPresentAndEnabled(db, "AccuratePercentiles")) {
    testError({$percentile: {p: [0.1, 0.6], input: "$str", method: "discrete"}},
              ErrorCodes.BadValue);
    testError({$median: {input: "$str", method: "discrete"}}, ErrorCodes.BadValue);
    testError({$percentile: {p: [0.1, 0.6], input: "$str", method: "continuous"}},
              ErrorCodes.BadValue);
    testError({$median: {input: "$str", method: "continuous"}}, ErrorCodes.BadValue);
}
// invalid expressions or variables for 'p'
testError({$percentile: {p: "$$ps", input: "$price", method: "approximate"}},
          7750301 /* non-numeric 'p' value in the variable */,
          {ps: "foo"} /* letSpec */);
testError({$percentile: {p: ["$price"], input: "$str", method: "approximate"}},
          7750300 /* non-const 'p' expression */);
testError({$percentile: {input: "$str", method: "approximate"}},
          ErrorCodes.IDLFailedToParse /* IDL required field error */);
testError({$median: {p: [0.1, 0.6], input: "$str", method: "approximate"}},
          ErrorCodes.IDLUnknownField);
