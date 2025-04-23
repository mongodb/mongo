/**
 * Test that $percentile and $median work as window functions w/ approximate methods.
 *  @tags: [
 *   requires_fcv_81,
 *   # $setParameter is not compatible with signed security tokens
 *   simulate_mongoq_incompatible,
 * ]
 */

import {
    seedWithTickerData,
    testAccumAgainstGroup
} from "jstests/aggregation/extras/window_function_helpers.js";
import {
    assertResultEqToVal,
    runSetWindowStage,
    testError
} from "jstests/aggregation/sources/setWindowFields/percentiles/percentile_util.js";
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

// Run the suite of partition and bounds tests against the $percentile function. Will run tests with
// removable and non-removable windows.
testAccumAgainstGroup(
    coll, "$percentile", [null, null], {p: [0.1, 0.6], input: "$price", method: "approximate"});
testAccumAgainstGroup(coll, "$median", null, {input: "$price", method: "approximate"});

// Test that $median and $percentile return null for windows which do not contain numeric values.
let results =
    runSetWindowStage(coll,
                      {$percentile: {p: [0.1, 0.6], input: "$str", method: "approximate"}},
                      {$median: {input: "$str", method: "approximate"}});
assertResultEqToVal({resultArray: results, percentile: [null, null], median: null});

// Test that an unbounded window calculates $percentile and $median correctly with approximate
// method.
results =
    runSetWindowStage(coll,
                      {$percentile: {p: [0.01, 0.99], input: "$price", method: "approximate"}},
                      {$median: {input: "$price", method: "approximate"}});
// Since our percentiles are 0.01 and 0.99 and our collection is small, we will always return the
// minimum and maximum value in the collection.
assertResultEqToVal(
    {resultArray: results, percentile: [minDoc.price, maxDoc.price], median: medianDoc.price});

// Test that an expression can be used for 'input'.
results = runSetWindowStage(
    coll,
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
results = runSetWindowStage(coll,
                            {$percentile: {p: "$$ps", input: "$price", method: "approximate"}},
                            {$median: {input: "$price", method: "approximate"}},
                            {ps: [0.01, 0.99]});
// Since our percentiles are 0.01 and 0.99 and our collection is small, we will always return the
// minimum and maximum value in the collection.
assertResultEqToVal(
    {resultArray: results, percentile: [minDoc.price, maxDoc.price], median: medianDoc.price});

// Test that a removable window calculates $percentile and $median correctly using an approximate
// method.
results = runSetWindowStage(
    coll,
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

// Invalid window specification.
testError(coll,
          {$percentile: {p: [0.1, 0.6], input: "$str", method: "approximate"}, window: [-1, 1]},
          ErrorCodes.FailedToParse);
testError(coll,
          {$median: {input: "$str", method: "approximate"}, window: [-1, 1]},
          ErrorCodes.FailedToParse);
testError(coll,
          {$percentile: {p: [0.6], input: "$str", method: "approximate"}, window: {documents: []}},
          ErrorCodes.FailedToParse);
testError(coll,
          {$median: {input: "$str", method: "approximate"}, window: {documents: []}},
          ErrorCodes.FailedToParse);

// Extra argument in the window function.
testError(coll,
          {$percentile: {p: [0.1, 0.6], input: "$str", method: "approximate"}, extra: "extra"},
          ErrorCodes.FailedToParse);
testError(coll,
          {$median: {input: "$str", method: "approximate"}, extra: "extra"},
          ErrorCodes.FailedToParse);

// Invalid input for the accumulators.
testError(coll, {$percentile: "not an object"}, 7429703);
testError(coll, {$median: "not an object"}, 7436100);

testError(
    coll, {$percentile: {p: [0.1, 0.6], input: "$str", method: false}}, ErrorCodes.TypeMismatch);
testError(coll, {$median: {input: "$str", method: false}}, ErrorCodes.TypeMismatch);

// invalid expressions or variables for 'p'
testError(coll,
          {$percentile: {p: "$$ps", input: "$price", method: "approximate"}},
          7750301 /* non-numeric 'p' value in the variable */,
          {ps: "foo"} /* letSpec */);
testError(coll,
          {$percentile: {p: ["$price"], input: "$str", method: "approximate"}},
          7750300 /* non-const 'p' expression */);
testError(coll,
          {$percentile: {input: "$str", method: "approximate"}},
          ErrorCodes.IDLFailedToParse /* IDL required field error */);
testError(coll,
          {$median: {p: [0.1, 0.6], input: "$str", method: "approximate"}},
          ErrorCodes.IDLUnknownField);

if (!FeatureFlagUtil.isPresentAndEnabled(db, "AccuratePercentiles")) {
    testError(coll,
              {$percentile: {p: [0.1, 0.6], input: "$str", method: "discrete"}},
              ErrorCodes.BadValue);
    testError(coll, {$median: {input: "$str", method: "discrete"}}, ErrorCodes.BadValue);
    testError(coll,
              {$percentile: {p: [0.1, 0.6], input: "$str", method: "continuous"}},
              ErrorCodes.BadValue);
    testError(coll, {$median: {input: "$str", method: "continuous"}}, ErrorCodes.BadValue);
}
