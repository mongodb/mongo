/**
 * Basic tests for the $firstN/$lastN accumulators.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");

const coll = db[jsTestName()];
coll.drop();

const isExactTopNEnabled = db.adminCommand({getParameter: 1, featureFlagExactTopNAccumulator: 1})
                               .featureFlagExactTopNAccumulator.value;

if (!isExactTopNEnabled) {
    // Verify that $firstN/$lastN cannot be used if the feature flag is set to false and ignore the
    // rest of the test.
    assert.commandFailedWithCode(coll.runCommand("aggregate", {
        pipeline:
            [{$group: {_id: {'st': '$state'}, firstValues: {$firstN: {output: '$sales', n: 2}}}}],
        cursor: {}
    }),
                                 15952);
    return;
}

// Basic correctness tests.
let docs = [];
const n = 3;
const kMaxSales = 20;
for (let i = 0; i < kMaxSales; i++) {
    if (i < 2) {
        docs.push({state: 'CA', sales: i * 10});
    }
    if (i < 3) {
        docs.push({state: 'AZ', sales: i * 10});
    }
    docs.push({state: 'NY', sales: i * 10});
}

assert.commandWorked(coll.insert(docs));

const actualFirstNResults =
    coll.aggregate([
            {$sort: {_id: 1}},
            {$group: {_id: '$state', sales: {$firstN: {output: "$sales", n: n}}}},
        ])
        .toArray();

const expectedFirstNResults =
    [{_id: "AZ", sales: [0, 10, 20]}, {_id: "CA", sales: [0, 10]}, {_id: "NY", sales: [0, 10, 20]}];

const actualLastNResults =
    coll.aggregate([
            {$sort: {_id: 1}},
            {$group: {_id: '$state', sales: {$lastN: {output: "$sales", n: n}}}},
        ])
        .toArray();

const expectedLastNResults = [
    {_id: "AZ", sales: [0, 10, 20]},
    {_id: "CA", sales: [0, 10]},
    {_id: "NY", sales: [170, 180, 190]}
];

// As these are unordered operators, we need to ensure we can deterministically test the values
// returned by firstN/lastN. As the output is not guaranteed to be in order, arrayEq is used
// instead.
arrayEq(expectedFirstNResults, actualFirstNResults);
arrayEq(expectedLastNResults, actualLastNResults);

// Reject non-integral values of n.
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline:
        [{$group: {_id: {'st': '$state'}, sales: {$firstN: {output: '$sales', n: 'string'}}}}],
    cursor: {}
}),
                             5787902);

assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{$group: {_id: {'st': '$state'}, sales: {$firstN: {output: '$sales', n: 3.2}}}}],
    cursor: {}
}),
                             5787903);

assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{$group: {_id: {'st': '$state'}, minSales: {$firstN: {output: '$sales', n: -1}}}}],
    cursor: {}
}),
                             5787908);

// Reject invalid specifications.

// Extra fields
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{
        $group: {
            _id: {'st': '$state'},
            sales: {$firstN: {output: '$sales', n: 2, randomField: "randomArg"}}
        }
    }],
    cursor: {}
}),
                             5787901);

// Missing arguments.
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{$group: {_id: {'st': '$state'}, sales: {$firstN: {output: '$sales'}}}}],
    cursor: {}
}),
                             5787906);

assert.commandFailedWithCode(
    coll.runCommand(
        "aggregate",
        {pipeline: [{$group: {_id: {'st': '$state'}, sales: {$minN: {n: 2}}}}], cursor: {}}),
    5787907);
})();