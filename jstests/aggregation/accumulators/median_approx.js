/**
 * Tests that the approximate median accumulator semantics matches the percentile semantics with the
 * field 'p':[0.5].
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
 * Tests for correctness without grouping. Confirms that $median computes the expected value and
 * also checks that $percentile for p=0.5 computes the same value, because $median is supposed to be
 * completely equivalent to the latter (e.g. we should not optimize $median independently of
 * $percentile).
 */
function testWithSingleGroup({docs, medianSpec, expectedResult, msg}) {
    coll.drop();
    coll.insertMany(docs);

    let medianArgs = medianSpec["$median"];
    const percentileSpec = {
        $percentile: {input: medianArgs.input, method: medianArgs.method, p: [0.5]}
    };

    const medianRes = coll.aggregate([{$group: {_id: null, p: medianSpec}}]).toArray();
    const percentileRes = coll.aggregate([{$group: {_id: null, p: percentileSpec}}]).toArray();

    assert.eq(expectedResult, medianRes[0].p, msg + ` result: ${tojson(medianRes)}`);

    // If all the data is non-numeric then the expected result is just null, and therefore cannot be
    // indexed into.
    assert.eq(percentileRes[0].p[0], medianRes[0].p, msg + ` result: ${tojson(medianRes)}`);
}

testWithSingleGroup({
    docs: [{x: 0}, {x: "non-numeric"}, {x: 1}, {no_x: 0}, {x: 2}],
    medianSpec: {$median: {input: "$x", method: "approximate"}},
    expectedResult: 1,
    msg: "Non-numeric data should be ignored"
});

testWithSingleGroup({
    docs: [{x: "non-numeric"}, {non_x: 1}],
    medianSpec: {$median: {input: "$x", method: "approximate"}},
    expectedResult: null,
    msg: "Median of completely non-numeric data."
});

function testWithMultipleGroups({docs, medianSpec, expectedResult, msg}) {
    coll.drop();
    coll.insertMany(docs);

    let medianArgs = medianSpec["$median"];
    const percentileSpec = {
        $percentile: {input: medianArgs.input, method: medianArgs.method, p: [0.5]}
    };

    const medianRes = coll.aggregate([{$group: {_id: null, p: medianSpec}}]).toArray();
    const percentileRes = coll.aggregate([{$group: {_id: null, p: percentileSpec}}]).toArray();

    assert.eq(medianRes.length, percentileRes.length);
    for (let i = 0; i < medianRes.length; i++) {
        assert.eq(expectedResult[i], medianRes[i].p, msg + ` result: ${tojson(medianRes)}`);
        assert.eq(percentileRes[i].p[0], medianRes[i].p, msg + ` result: ${tojson(medianRes)}`);
    }
}

testWithMultipleGroups({
    docs: [{k: 0, x: 2}, {k: 0, x: 1}, {k: 1, x: 2}, {k: 2}, {k: 0, x: "str"}, {k: 1, x: 0}],
    medianSpec: {$median: {input: "$x", method: "approximate"}},
    expectedResult: [/* k:0 */ 1, /* k:1 */ 0, /* k:2 */ null],
    msg: "Median of multiple groups"
});
})();
