/**
 * Tests for the discrete percentile accumulator semantics.
 * @tags: [
 *   requires_fcv_81,
 *   featureFlagAccuratePercentiles
 * ]
 */
import {
    testLargeUniformDataset,
    testLargeUniformDataset_Decimal,
    testLargeUniformDataset_WithInfinities,
    testWithMultipleGroups,
    testWithSingleGroup,
} from "jstests/aggregation/libs/percentiles_util.js";

const coll = db[jsTestName()];

/**
 * Tests for correctness without grouping. Each group gets its own accumulator so we can
 * validate the basic $percentile functionality using a single group.
 */
testWithSingleGroup({
    coll: coll,
    docs: [{x: 0}, {x: "non-numeric"}, {x: 1}, {no_x: 0}, {x: 2}],
    percentileSpec: {$percentile: {p: [0.5], input: "$x", method: "discrete"}},
    expectedResult: [1],
    msg: "Non-numeric data should be ignored",
});

testWithSingleGroup({
    coll: coll,
    docs: [{x: "non-numeric"}, {no_x: 0}, {x: new Date()}, {x: [42, 43]}, {x: null}, {x: NaN}],
    percentileSpec: {$percentile: {p: [0.5], input: "$x", method: "discrete"}},
    expectedResult: [null],
    msg: "Single percentile of completely non-numeric data",
});

testWithSingleGroup({
    coll: coll,
    docs: [{x: "non-numeric"}, {no_x: 0}, {x: new Date()}, {x: [42, 43]}, {x: null}, {x: NaN}],
    percentileSpec: {$percentile: {p: [0.5, 0.9], input: "$x", method: "discrete"}},
    expectedResult: [null, null],
    msg: "Multiple percentiles of completely non-numeric data",
});

testWithSingleGroup({
    coll: coll,
    docs: [{x: 10}, {x: 5}, {x: 27}],
    percentileSpec: {$percentile: {p: [0], input: "$x", method: "discrete"}},
    expectedResult: [5],
    msg: "Minimum percentile",
});

testWithSingleGroup({
    coll: coll,
    docs: [{x: 10}, {x: 5}, {x: 27}],
    percentileSpec: {$percentile: {p: [1], input: "$x", method: "discrete"}},
    expectedResult: [27],
    msg: "Maximum percentile",
});

testWithSingleGroup({
    coll: coll,
    docs: [{x: 0}, {x: 1}, {x: 2}],
    percentileSpec: {$percentile: {p: [0.5, 0.9, 0.1], input: "$x", method: "discrete"}},
    expectedResult: [1, 2, 0],
    msg: "Multiple percentiles",
});

testWithSingleGroup({
    coll: coll,
    docs: [{x: -Infinity}, {x: 0}, {x: 1}, {x: 2}, {x: Infinity}, {x: Infinity}, {x: Infinity}],
    percentileSpec: {$percentile: {p: [0.5, 0.9, 0.1], input: "$x", method: "discrete"}},
    expectedResult: [2, Infinity, -Infinity],
    msg: "Multiple percentiles with infinities",
});

testWithSingleGroup({
    coll: coll,
    docs: [{x: 0}, {x: 1}, {x: 2}],
    percentileSpec: {$percentile: {p: "$$ps", input: "$x", method: "discrete"}},
    letSpec: {ps: [0.5, 0.9, 0.1]},
    expectedResult: [1, 2, 0],
    msg: "Multiple percentiles using variable in the percentile spec for the whole array",
});

testWithSingleGroup({
    coll: coll,
    docs: [{x: 0}, {x: 1}, {x: 2}],
    percentileSpec: {$percentile: {p: ["$$p90"], input: "$x", method: "discrete"}},
    letSpec: {p90: 0.9},
    expectedResult: [2],
    msg: "Single percentile using variable in the percentile spec for the array elements",
});

testWithSingleGroup({
    coll: coll,
    docs: [{x: 0}, {x: 1}, {x: 2}],
    percentileSpec: {
        $percentile: {p: {$concatArrays: [[0.1, 0.5], ["$$p90"]]}, input: "$x", method: "discrete"},
    },
    letSpec: {p90: 0.9},
    expectedResult: [0, 1, 2],
    msg: "Multiple percentiles using const expression in the percentile spec",
});

testWithSingleGroup({
    coll: coll,
    docs: [{x: 0}, {x: 1}, {x: 2}],
    percentileSpec: {$percentile: {p: "$$ps", input: {$add: [42, "$x"]}, method: "discrete"}},
    letSpec: {ps: [0.5, 0.9, 0.1]},
    expectedResult: [42 + 1, 42 + 2, 42 + 0],
    msg: "Multiple percentiles using expression as input",
});

/**
 * Tests for correctness with grouping on $k and computing the percentile on $x.
 */
testWithMultipleGroups({
    coll: coll,
    docs: [{k: 0, x: 0}, {k: 0, x: 1}, {k: 1, x: 2}, {k: 2}, {k: 0, x: "str"}, {k: 1, x: 0}],
    percentileSpec: {$percentile: {p: [0.9], input: "$x", method: "discrete"}},
    expectedResult: [/* k:0 */ [1], /* k:1 */ [2], /* k:2 */ [null]],
    msg: "Multiple groups",
});

/**
 * The tests above use tiny datasets. The following tests use more data. We create the data with
 * Random.rand() which produces a uniform distribution in [0.0, 1.0) (for testing with other
 * data distributions see C++ unit tests for discrete).
 */
Random.setRandomFixtureSeed();
// 'error' should be specified as zero since discrete should be accurate
const accuracyError = 0;

let samples = [];
for (let i = 0; i < 10000; i++) {
    samples.push(Random.rand());
}
let sortedSamples = [].concat(samples);
sortedSamples.sort((a, b) => a - b);

const p = [0.0, 0.001, 0.01, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.99, 0.999, 1.0];

testLargeUniformDataset(coll, samples, sortedSamples, p, accuracyError, "discrete");

testLargeUniformDataset_WithInfinities(coll, samples, sortedSamples, p, accuracyError, "discrete");

// TODO SERVER-91956 improve precision so that this test succeeds.
// // Same dataset but using Decimal128 type.
// // testLargeUniformDataset_Decimal(coll, samples, sortedSamples, p, accuracyError,
// "discrete")
