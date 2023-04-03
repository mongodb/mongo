/**
 * Tests for the approximate percentile accumulator semantics.
 * @tags: [
 *   requires_fcv_70,
 *   featureFlagApproxPercentiles
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");

const coll = db[jsTestName()];

/**
 * Tests for correctness without grouping. Each group gets its own accumulator so we can validate
 * the basic $percentile functionality using a single group.
 */
function testWithSingleGroup({docs, percentileSpec, expectedResult, msg}) {
    coll.drop();
    coll.insertMany(docs);
    const res = coll.aggregate([{$group: {_id: null, p: percentileSpec}}]).toArray();

    // For $percentile the result should be ordered to match the spec, so assert exact equality.
    assert.eq(expectedResult, res[0].p, msg + `; Result: ${tojson(res)}`);
}

testWithSingleGroup({
    docs: [{x: 0}, {x: "non-numeric"}, {x: 1}, {no_x: 0}, {x: 2}],
    percentileSpec: {$percentile: {p: [0.5], input: "$x", method: "approximate"}},
    expectedResult: [1],
    msg: "Non-numeric data should be ignored"
});

testWithSingleGroup({
    docs: [{x: "non-numeric"}, {no_x: 0}, {x: new Date()}, {x: [42, 43]}, {x: null}, {x: NaN}],
    percentileSpec: {$percentile: {p: [0.5], input: "$x", method: "approximate"}},
    expectedResult: [null],
    msg: "Single percentile of completely non-numeric data"
});

testWithSingleGroup({
    docs: [{x: "non-numeric"}, {no_x: 0}, {x: new Date()}, {x: [42, 43]}, {x: null}, {x: NaN}],
    percentileSpec: {$percentile: {p: [0.5, 0.9], input: "$x", method: "approximate"}},
    expectedResult: [null, null],
    msg: "Multiple percentiles of completely non-numeric data"
});

testWithSingleGroup({
    docs: [{x: 10}, {x: 5}, {x: 27}],
    percentileSpec: {$percentile: {p: [0], input: "$x", method: "approximate"}},
    expectedResult: [5],
    msg: "Minimun percentile"
});

testWithSingleGroup({
    docs: [{x: 10}, {x: 5}, {x: 27}],
    percentileSpec: {$percentile: {p: [1], input: "$x", method: "approximate"}},
    expectedResult: [27],
    msg: "Maximum percentile"
});

testWithSingleGroup({
    docs: [{x: 0}, {x: 1}, {x: 2}],
    percentileSpec: {$percentile: {p: [0.5, 0.9, 0.1], input: "$x", method: "approximate"}},
    expectedResult: [1, 2, 0],
    msg: "Multiple percentiles"
});

function testWithMultipleGroups({docs, percentileSpec, expectedResult, msg}) {
    coll.drop();
    coll.insertMany(docs);
    const res =
        coll.aggregate([{$group: {_id: "$k", p: percentileSpec}}, {$sort: {_id: 1}}]).toArray();

    // For $percentile the result should be ordered to match the spec, so assert exact equality.
    for (let i = 0; i < res.length; i++) {
        assert.eq(expectedResult[i], res[i].p, msg + ` result: ${tojson(res)}`);
    }
}

testWithMultipleGroups({
    docs: [{k: 0, x: 0}, {k: 0, x: 1}, {k: 1, x: 2}, {k: 2}, {k: 0, x: "str"}, {k: 1, x: 0}],
    percentileSpec: {$percentile: {p: [0.9], input: "$x", method: "approximate"}},
    expectedResult: [/* k:0 */[1], /* k:1 */[2], /* k:2 */[null]],
    msg: "Multiple groups"
});

/**
 * The tests above use tiny datasets where t-digest would create a centroid per sample and will be
 * always precise. The following tests use more data. We create the data with Random.rand() which
 * produces a uniform distribution in [0.0, 1.0) (for testing with other data distributions see C++
 * unit tests for TDigest).
 */
function findRank(sorted, value) {
    for (let i = 0; i < sorted.length; i++) {
        if (sorted[i] >= value) {
            return i;
        }
    }
    return -1;
}

// 'sorted' is expected to contain a sorted array of all _numeric_ samples that are used to compute
// the percentile(s). 'error' should be specified as a percentage point. While t-digest is expected
// to have better accuracy for the extreme percentiles, we chech the error uniformly in these tests
// because on uniform distribution with our chosen seed, the error happens to be super low across
// the board.
function testWithAccuracyError({docs, percentileSpec, sorted, error, msg}) {
    coll.drop();
    coll.insertMany(docs);
    const res = coll.aggregate([{$group: {_id: null, p: percentileSpec}}]).toArray();

    for (let i = 0; i < res[0].p.length; i++) {
        const p = percentileSpec.$percentile.p[i];
        const computedRank = findRank(sorted, res[0].p[i]);
        const trueRank = Math.max(0, p * sorted.length - 1);
        assert.lte(Math.abs(trueRank - computedRank),
                   error * sorted.length,
                   msg +
                       `; Error is too high for p: ${p}. Computed: ${res[0].p[i]} with rank ${
                           computedRank} but the true rank is ${trueRank} in ${tojson(res)}`);
    }
}

// The seed is arbitrary but the accuracy error has been empirically determined based on the
// generated samples with _this_ seed.
Random.setRandomSeed(20230328);
const accuracyError = 0.001;

let samples = [];
for (let i = 0; i < 10000; i++) {
    samples.push(Random.rand());
}
let sortedSamples = [].concat(samples);
sortedSamples.sort((a, b) => a - b);

const p = [0.0, 0.001, 0.01, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.99, 0.999, 1.0];

(function testLargeUniformDataset() {
    let docs = [];
    for (let i = 0; i < samples.length; i++) {
        docs.push({_id: i, x: samples[i]});
    }
    testWithAccuracyError({
        docs: docs,
        percentileSpec: {$percentile: {p: p, input: "$x", method: "approximate"}},
        sorted: sortedSamples,
        msg: "Single group of uniformly distributed data",
        error: accuracyError
    });
})();

(function testLargeUniformDataset_WithInfinities() {
    let docs = [];
    for (let i = 0; i < samples.length; i++) {
        docs.push({_id: i * 10, x: samples[i]});
        if (i % 2000 == 0) {
            docs.push({_id: i * 10 + 1, x: Infinity});
            docs.push({_id: i * 10 + 2, x: -Infinity});
        }
    }

    let sortedSamplesWithInfinities =
        Array(5).fill(-Infinity).concat(sortedSamples).concat(Array(5).fill(Infinity));

    testWithAccuracyError({
        docs: docs,
        percentileSpec: {$percentile: {p: p, input: "$x", method: "approximate"}},
        sorted: sortedSamplesWithInfinities,
        msg: "Single group of uniformly distributed data with infinite values",
        error: accuracyError
    });
})();

// Same dataset but using Decimal128 type.
(function testLargeUniformDataset_Decimal() {
    let docs = [];
    for (let i = 0; i < samples.length; i++) {
        docs.push({_id: i * 10, x: NumberDecimal(samples[i])});
        if (i % 1000 == 0) {
            docs.push({_id: i * 10 + 1, x: NumberDecimal(NaN)});
            docs.push({_id: i * 10 + 2, x: NumberDecimal(-NaN)});
        }
    }
    testWithAccuracyError({
        docs: docs,
        percentileSpec: {$percentile: {p: p, input: "$x", method: "approximate"}},
        sorted: sortedSamples,
        msg: "Single group of uniformly distributed Decimal128 data",
        error: accuracyError
    });
})();
})();
