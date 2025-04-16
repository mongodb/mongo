// @tags: [featureFlagAccuratePercentiles]

import {
    testLargeUniformDataset,
    testLargeUniformDataset_WithInfinities,
    testWithMultipleGroups,
    testWithSingleGroup
} from "jstests/aggregation/libs/percentiles_util.js";

// Tests that the accurate methods of percentile (discrete and continuous) function correctly even
// when memory limits are lowered such that they spill to disk at the group and/or accumulator
// levels.
const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");
const coll = db.agg_configurable_memory_limit;

// Test that the percentile accumulator produces correct results for accurate percentiles even when
// forced to spill to disk at both the group and accumulator levels.
(function testGroupAndAccumulatorSpilling() {
    // Force spill at group level for all tests.
    runSpillingTestsWithServerParameter(
        {setParameter: 1, internalDocumentSourceGroupMaxMemoryBytes: 1});

    // Force spill at accumulator level for all tests (group level spilling will still also occur).
    runSpillingTestsWithServerParameter(
        {setParameter: 1, internalQueryMaxPercentileAccumulatorBytes: 1});
}());

function runSpillingTestsWithServerParameter(setParameterObject) {
    assert.commandWorked(db.adminCommand(setParameterObject));

    /**
     * Tests for correctness with single group.
     */

    testWithSingleGroup({
        coll: coll,
        docs: [{x: 0}, {x: "non-numeric"}, {x: 1}, {x: 2}, {x: 3}, {x: 4}, {x: 5}],
        percentileSpec: {$percentile: {p: [0, 0.5, 1], input: "$x", method: "discrete"}},
        expectedResult: [0, 2, 5],
        msg: "Non-numeric data should be ignored"
    });

    testWithSingleGroup({
        coll: coll,
        docs: [{x: 0}, {x: "non-numeric"}, {x: 1}, {x: 2}, {x: 3}, {x: 4}, {x: 5}],
        percentileSpec: {$percentile: {p: [0, 0.5, 1], input: "$x", method: "continuous"}},
        expectedResult: [0, 2.5, 5],
        msg: "Non-numeric data should be ignored"
    });

    testWithSingleGroup({
        coll: coll,
        docs: [{x: -Infinity}, {x: 0}, {x: 1}, {x: 2}, {x: Infinity}, {x: Infinity}, {x: Infinity}],
        percentileSpec: {$percentile: {p: [0.5, 0.9, 0.1], input: "$x", method: "discrete"}},
        expectedResult: [2, Infinity, -Infinity],
        msg: "Infinities should not be ignored"
    });

    testWithSingleGroup({
        coll: coll,
        docs: [{x: -Infinity}, {x: 0}, {x: 1}, {x: 2}, {x: Infinity}, {x: Infinity}, {x: Infinity}],
        percentileSpec: {$percentile: {p: [0.5, 0.9, 0.1], input: "$x", method: "continuous"}},
        expectedResult: [2, Infinity, -Infinity],
        msg: "Infinities should not be ignored"
    });

    /**
     * Tests for correctness with grouping on $k and computing the percentile on $x.
     */

    let multipleGroupsDocs = [
        {k: 0, x: 0},
        {k: 0, x: 1},
        {k: 0, x: Infinity},
        {k: 0, x: 2},
        {k: 1, x: -Infinity},
        {k: 1, x: 5},
        {k: 1, x: 6},
        {k: 2, x: "not a number"},
        {k: 2, x: "not a number"},
        {k: 2, x: Infinity},
        {k: "three", x: 10},
        {k: "three", x: 20}
    ];

    testWithMultipleGroups({
        coll: coll,
        docs: multipleGroupsDocs,
        percentileSpec: {$percentile: {p: [0.5], input: "$x", method: "discrete"}},
        expectedResult: [/* k:0 */[1], /* k:1 */[5], /* k:2 */[Infinity], /* k:"three" */[10]],
        msg:
            "Percentile should be calculated for each of several groups, ignoring non-numeric data and including infinities."
    });

    testWithMultipleGroups({
        coll: coll,
        docs: multipleGroupsDocs,
        percentileSpec: {$percentile: {p: [0.5], input: "$x", method: "continuous"}},
        expectedResult: [/* k:0 */[1.5], /* k:1 */[5], /* k:2 */[Infinity], /* k:"three" */[15]],
        msg:
            "Percentile should be calculated for each of several groups, ignoring non-numeric data and including infinities."
    });

    /**
     * Tests for correctness with much larger datasets, with and without infinity values.
     */

    Random.setRandomFixtureSeed();
    // 'error' should be specified as zero since discrete and continuous should be accurate.
    const accuracyError = 0;

    let samples = [];
    for (let i = 0; i < 10000; i++) {
        samples.push(Random.rand());
    }
    let sortedSamples = [].concat(samples);
    sortedSamples.sort((a, b) => a - b);

    // TODO SERVER-91956: Once precision is improved, add tests with higher precision decimal p
    // values.
    const p = [0.0, 0.001, 0.01, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.99, 0.999, 1.0];

    testLargeUniformDataset(coll, samples, sortedSamples, p, accuracyError, "discrete");

    testLargeUniformDataset_WithInfinities(
        coll, samples, sortedSamples, p, accuracyError, "discrete");

    testLargeUniformDataset(coll, samples, sortedSamples, p, accuracyError, "continuous");

    testLargeUniformDataset_WithInfinities(
        coll, samples, sortedSamples, p, accuracyError, "continuous");
}

MongoRunner.stopMongod(conn);
