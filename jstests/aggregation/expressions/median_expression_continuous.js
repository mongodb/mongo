/**
 * Tests that the discrete median expression semantics match the percentile expression semantics
 * with the field 'p':[0.5].
 * @tags: [
 *   requires_fcv_81,
 *   featureFlagAccuratePercentiles,
 * ]
 */
import {testWithProjectMedian} from "jstests/aggregation/libs/percentiles_util.js";

const coll = db[jsTestName()];

/**
 * Tests with input as single expression which evaluates to an array.
 */
testWithProjectMedian({
    coll: coll,
    doc: {x: [0, 2]},
    medianSpec: {$median: {input: "$x", method: "continuous"}},
    expectedResult: 1,
    msg: "Continuous interpolation should allow result not in original dataset",
});

testWithProjectMedian({
    coll: coll,
    doc: {x: [0, "non-numeric", 1, 2], no_x: 0},
    medianSpec: {$median: {input: "$x", method: "continuous"}},
    expectedResult: 1,
    msg: "Non-numeric data should be ignored in input which evaluates to an array",
});

testWithProjectMedian({
    coll: coll,
    doc: {x: ["non-numeric", [1, 2, 3]]},
    medianSpec: {$median: {input: "$x", method: "continuous"}},
    expectedResult: null,
    msg: "Median of completely non-numeric data in input which evaluates to an array",
});

/**
 * Tests with input as an array of expressions.
 */
testWithProjectMedian({
    coll: coll,
    doc: {x: 0, x1: "non-numeric", x2: 1, x3: 2},
    medianSpec: {$median: {input: ["$x", "$x1", "$x2", "$x3"], method: "continuous"}},
    expectedResult: 1,
    msg: "Non-numeric data should be ignored in input passed in as an array",
});

testWithProjectMedian({
    coll: coll,
    doc: {x: "non-numeric", x1: "hello"},
    medianSpec: {$median: {input: ["$x", "$x1"], method: "continuous"}},
    expectedResult: null,
    msg: "Median of completely non-numeric data in input passed in as an array",
});

/**
 * Tests with input as a scalar.
 */
testWithProjectMedian({
    coll: coll,
    doc: {x: 1, x1: "hello"},
    medianSpec: {$median: {input: "$x1", method: "continuous"}},
    expectedResult: null,
    msg: "Median of completely non-numeric data with input as a scalar",
});
