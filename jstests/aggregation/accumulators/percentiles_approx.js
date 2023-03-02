/**
 * Tests for the approximate percentile accumulator semantics.
 * @tags: [
 *   featureFlagApproxPercentiles,
 *   # sharded collections aren't supported yet
 *   assumes_unsharded_collection,
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
    assert.eq(expectedResult, res[0].p, msg + ` result: ${tojson(res)}`);
}

testWithSingleGroup({
    docs: [{x: 0}, {x: "non-numeric"}, {x: 1}, {no_x: 0}, {x: 2}],
    percentileSpec: {$percentile: {p: [0.5], input: "$x", algorithm: "approximate"}},
    expectedResult: [1],
    msg: "Non-numeric data should be ignored"
});

testWithSingleGroup({
    docs: [{x: "non-numeric"}, {no_x: 0}],
    percentileSpec: {$percentile: {p: [0.5], input: "$x", algorithm: "approximate"}},
    expectedResult: null,
    msg: "Percentile of completely non-numeric data"
});

testWithSingleGroup({
    docs: [{x: "non-numeric"}, {no_x: 0}],
    percentileSpec: {$percentile: {p: [0.5, 0.9], input: "$x", algorithm: "approximate"}},
    expectedResult: null,
    msg: "Multiple percentiles of completely non-numeric data"
});

testWithSingleGroup({
    docs: [{x: 10}, {x: 5}, {x: 27}],
    percentileSpec: {$percentile: {p: [0], input: "$x", algorithm: "approximate"}},
    expectedResult: [5],
    msg: "Minimun percentile"
});

testWithSingleGroup({
    docs: [{x: 10}, {x: 5}, {x: 27}],
    percentileSpec: {$percentile: {p: [1], input: "$x", algorithm: "approximate"}},
    expectedResult: [27],
    msg: "Maximum percentile"
});

testWithSingleGroup({
    docs: [{x: 0}, {x: 1}, {x: 2}],
    percentileSpec: {$percentile: {p: [0.5, 0.9, 0.1], input: "$x", algorithm: "approximate"}},
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
    percentileSpec: {$percentile: {p: [0.9], input: "$x", algorithm: "approximate"}},
    expectedResult: [/* k:0 */[1], /* k:1 */[2], /* k:2 */ null],
    msg: "Multiple groups"
});
})();
