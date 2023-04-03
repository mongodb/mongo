/**
 * Tests that the approximate median expression semantics match the percentile expression semantics
 * with the field 'p':[0.5].
 * @tags: [
 *   requires_fcv_70,
 *   featureFlagApproxPercentiles
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");

const coll = db[jsTestName()];

function testWithProject({doc, medianSpec, expectedResult, msg}) {
    coll.drop();
    coll.insert(doc);

    let medianArgs = medianSpec["$median"];
    const percentileSpec = {
        $percentile: {input: medianArgs.input, method: medianArgs.method, p: [0.5]}
    };

    const medianRes = coll.aggregate([{$project: {p: medianSpec}}]).toArray();
    const percentileRes = coll.aggregate([{$project: {p: percentileSpec}}]).toArray();

    assert.eq(expectedResult, medianRes[0].p, msg + ` result: ${tojson(medianRes)}`);

    // If all the data is non-numeric then the expected result is just null, and therefore cannot be
    // indexed into.
    assert.eq(percentileRes[0].p[0], medianRes[0].p, msg + ` result: ${tojson(medianRes)}`);
}

/**
 * Tests with input as single expression which evaluates to an array.
 */
testWithProject({
    doc: {x: [0, "non-numeric", 1, 2], no_x: 0},
    medianSpec: {$median: {input: "$x", method: "approximate"}},
    expectedResult: 1,
    msg: "Non-numeric data should be ignored in input which evaluates to an array"
});

testWithProject({
    doc: {x: ["non-numeric", [1, 2, 3]]},
    medianSpec: {$median: {input: "$x", method: "approximate"}},
    expectedResult: null,
    msg: "Median of completely non-numeric data in input which evaluates to an array"
});

/**
 * Tests with input as an array of expressions.
 */
testWithProject({
    doc: {x: 0, x1: "non-numeric", x2: 1, x3: 2},
    medianSpec: {$median: {input: ["$x", "$x1", "$x2", "$x3"], method: "approximate"}},
    expectedResult: 1,
    msg: "Non-numeric data should be ignored in input passed in as an array"
});

testWithProject({
    doc: {x: "non-numeric", x1: "hello"},
    medianSpec: {$median: {input: ["$x", "$x1"], method: "approximate"}},
    expectedResult: null,
    msg: "Median of completely non-numeric data in input passed in as an array"
});

/**
 * Tests with input as a scalar.
 */
testWithProject({
    doc: {x: 1, x1: "hello"},
    medianSpec: {$median: {input: "$x1", method: "approximate"}},
    expectedResult: null,
    msg: "Median of completely non-numeric data with input as a scalar"
});
})();
