/**
 * Tests for the approximate percentile expression semantics.
 * @tags: [
 *   requires_fcv_70,
 *   featureFlagApproxPercentiles
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");

const coll = db[jsTestName()];

function testWithProject({doc, percentileSpec, expectedResult, msg}) {
    coll.drop();
    coll.insert(doc);
    const res = coll.aggregate([{$project: {p: percentileSpec}}]).toArray();
    // For $percentile the result should be ordered to match the spec, so assert exact equality.
    assert.eq(expectedResult, res[0].p, msg + ` result: ${tojson(res)}`);
}

/**
 * Tests with input as a single expression which evaluates to an array.
 */
testWithProject({
    doc: {x: [0, "non-numeric", 1, 2]},
    percentileSpec: {$percentile: {p: [0.5], input: "$x", method: "approximate"}},
    expectedResult: [1],
    msg: "Non-numeric data in input field which evaluates to an array should be ignored"
});

testWithProject({
    doc: {x: [10, 5, 27], x1: 5},
    percentileSpec: {$percentile: {p: [0], input: "$x", method: "approximate"}},
    expectedResult: [5],
    msg: "Minimun percentile"
});

testWithProject({
    doc: {x: [0, 1, 2]},
    percentileSpec: {$percentile: {p: [0.5, 0.9, 0.1], input: "$x", method: "approximate"}},
    expectedResult: [1, 2, 0],
    msg: "Multiple percentiles when input field evaluates to an array"
});

/**
 * Tests with input as an array of expressions.
 */
testWithProject({
    doc: {x: 0, x1: "non-numeric", x2: 1, x3: 2},
    percentileSpec:
        {$percentile: {p: [0.5], input: ["$x", "$x1", "$x2", "$x3"], method: "approximate"}},
    expectedResult: [1],
    msg: "Non-numeric data in input field passed in as an array should be ignored"
});

testWithProject({
    doc: {x: "non-numeric"},
    percentileSpec: {$percentile: {p: [0.5], input: ["$x"], method: "approximate"}},
    expectedResult: [null],
    msg: "Percentile of completely non-numeric data when input field is an array"
});

testWithProject({
    doc: {x: "non-numeric", x1: "also non-numeric", x2: [1, 2, 3]},
    percentileSpec:
        {$percentile: {p: [0.5, 0.9], input: ["$x", "$x1", "$x2"], method: "approximate"}},
    expectedResult: [null, null],
    msg: "Multiple percentiles of completely non-numeric data in input field passed as an array"
});

testWithProject({
    doc: {x: 10, x1: 5, x2: 27},
    percentileSpec: {$percentile: {p: [1], input: ["$x", "$x1", "$x2"], method: "approximate"}},
    expectedResult: [27],
    msg: "Maximum percentile"
});

testWithProject({
    doc: {x: 0, x1: 1, x2: 2},
    percentileSpec:
        {$percentile: {p: [0.5, 0.9, 0.1], input: ["$x", "$x1", "$x2"], method: "approximate"}},
    expectedResult: [1, 2, 0],
    msg: "Multiple percentiles when input field is passed as an array"
});

/**
 * Tests with input as a single expression.
 */
testWithProject({
    doc: {x: 0, x1: 1, x2: 2},
    percentileSpec: {$percentile: {p: [0.5, 0.9, 0.1], input: "$x", method: "approximate"}},
    expectedResult: [0, 0, 0],
    msg: "Multiple percentiles when single input expression resolves to a numeric scalar"
});

testWithProject({
    doc: {x: 0, x1: "non-numeric", x2: 2},
    percentileSpec: {$percentile: {p: [0.5, 0.9, 0.1], input: "$x1", method: "approximate"}},
    expectedResult: [null, null, null],
    msg: "Multiple percentiles when single input expression resolves to a non-numeric scalar"
});
})();
