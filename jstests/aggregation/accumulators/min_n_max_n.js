/**
 * Basic tests for the $minN/$maxN accumulators.
 */
(function() {
"use strict";

const coll = db[jsTestName()];
coll.drop();

const isExactTopNEnabled = db.adminCommand({getParameter: 1, featureFlagExactTopNAccumulator: 1})
                               .featureFlagExactTopNAccumulator.value;

if (!isExactTopNEnabled) {
    // Verify that $minN/$maxN cannot be used if the feature flag is set to false and ignore the
    // rest of the test.
    assert.commandFailedWithCode(coll.runCommand("aggregate", {
        pipeline: [{$group: {_id: {'st': '$state'}, minSales: {$minN: {output: '$sales', n: 2}}}}],
        cursor: {}
    }),
                                 15952);
    return;
}

// Basic correctness tests.
let docs = [];
const n = 4;
const states = [{state: 'CA', sales: 10}, {state: 'NY', sales: 7}, {state: 'TX', sales: 4}];
let expectedMinNResults = [];
let expectedMaxNResults = [];
for (const stateDoc of states) {
    const state = stateDoc['state'];
    const sales = stateDoc['sales'];
    let minArr = [];
    let maxArr = [];
    for (let i = 1; i <= sales; ++i) {
        const amount = i * 100;
        docs.push({state: state, sales: amount});

        // Record the lowest/highest 'n' values.
        if (i < n + 1) {
            minArr.push(amount);
        }
        if (sales - n < i) {
            maxArr.push(amount);
        }
    }
    expectedMinNResults.push({_id: state, minSales: minArr});

    // Reverse 'maxArr' results since $maxN outputs results in descending order.
    expectedMaxNResults.push({_id: state, maxSales: maxArr.reverse()});
}

assert.commandWorked(coll.insert(docs));

// Note that the output documents are sorted by '_id' so that we can compare actual groups against
// expected groups (we cannot perform unordered comparison because order matters for $minN/$maxN).
const actualMinNResults =
    coll.aggregate([
            {$group: {_id: '$state', minSales: {$minN: {output: '$sales', n: n}}}},
            {$sort: {_id: 1}}
        ])
        .toArray();
assert.eq(expectedMinNResults, actualMinNResults);

const actualMaxNResults =
    coll.aggregate([
            {$group: {_id: '$state', maxSales: {$maxN: {output: '$sales', n: n}}}},
            {$sort: {_id: 1}}
        ])
        .toArray();
assert.eq(expectedMaxNResults, actualMaxNResults);

// Verify that we can dynamically compute 'n' based on the group key for $group.
const groupKeyNExpr = {
    $cond: {if: {$eq: ['$st', 'CA']}, then: 10, else: 4}
};
const dynamicMinNResults =
    coll.aggregate([{
            $group: {_id: {'st': '$state'}, minSales: {$minN: {output: '$sales', n: groupKeyNExpr}}}
        }])
        .toArray();

// Verify that the 'CA' group has 10 results, while all others have only 4.
for (const result of dynamicMinNResults) {
    assert(result.hasOwnProperty('_id'), tojson(result));
    const groupKey = result['_id'];
    assert(groupKey.hasOwnProperty('st'), tojson(groupKey));
    const state = groupKey['st'];
    assert(result.hasOwnProperty('minSales'), tojson(result));
    const salesArray = result['minSales'];
    if (state === 'CA') {
        assert.eq(salesArray.length, 10, tojson(salesArray));
    } else {
        assert.eq(salesArray.length, 4, tojson(salesArray));
    }
}

// Error cases

// Cannot reference the group key in $minN when using $bucketAuto.
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{
        $bucketAuto: {
            groupBy: "$state",
            buckets: 2,
            output: {minSales: {$minN: {output: '$sales', n: groupKeyNExpr}}}
        }
    }],
    cursor: {}
}),
                             4544714);

// Reject non-integral/negative values of n.
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline:
        [{$group: {_id: {'st': '$state'}, minSales: {$minN: {output: '$sales', n: 'string'}}}}],
    cursor: {}
}),
                             5787902);

assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{$group: {_id: {'st': '$state'}, minSales: {$minN: {output: '$sales', n: 3.2}}}}],
    cursor: {}
}),
                             5787903);

assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{$group: {_id: {'st': '$state'}, minSales: {$minN: {output: '$sales', n: -1}}}}],
    cursor: {}
}),
                             5787908);

// Reject invalid specifications.

// Missing arguments.
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{$group: {_id: {'st': '$state'}, minSales: {$minN: {output: '$sales'}}}}],
    cursor: {}
}),
                             5787906);

assert.commandFailedWithCode(
    coll.runCommand(
        "aggregate",
        {pipeline: [{$group: {_id: {'st': '$state'}, minSales: {$minN: {n: 2}}}}], cursor: {}}),
    5787907);

// Extra field.
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{
        $group: {
            _id: {'st': '$state'},
            minSales: {$minN: {output: '$sales', n: 2, randomField: "randomArg"}}
        }
    }],
    cursor: {}
}),
                             5787901);
})();
