/**
 * Tests basic functionality of pushing $group into the find layer.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const featureEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagSBEGroupPushdown: 1}))
        .featureFlagSBEGroupPushdown.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the sbe group pushdown feature flag is disabled");
    return;
}

const coll = db.group_pushdown;
coll.drop();

assert.commandWorked(coll.insert([
    {"_id": 1, "item": "a", "price": 10, "quantity": 2, "date": ISODate("2014-01-01T08:00:00Z")},
    {"_id": 2, "item": "b", "price": 20, "quantity": 1, "date": ISODate("2014-02-03T09:00:00Z")},
    {"_id": 3, "item": "a", "price": 5, "quantity": 5, "date": ISODate("2014-02-03T09:05:00Z")},
    {"_id": 4, "item": "b", "price": 10, "quantity": 10, "date": ISODate("2014-02-15T08:00:00Z")},
    {"_id": 5, "item": "c", "price": 5, "quantity": 10, "date": ISODate("2014-02-15T09:05:00Z")},
]));

let assertGroupPushdown = function(coll, pipeline, expectedResults, expectedGroupCountInExplain) {
    const explain = coll.explain().aggregate(pipeline);
    // When $group isnever pushed down it be present as a stage in the 'winningPlan' of $cursor.
    assert.eq(expectedGroupCountInExplain, getAggPlanStages(explain, "GROUP").length, explain);

    let results = coll.aggregate(pipeline).toArray();
    assert.sameMembers(results, expectedResults);
};

let assertNoGroupPushdown = function(coll, pipeline, expectedResults, options = {}) {
    const explain = coll.explain().aggregate(pipeline, options);
    assert.eq(null, getAggPlanStage(explain, "GROUP"), explain);

    let resultNoGroupPushdown = coll.aggregate(pipeline, options).toArray();
    assert.sameMembers(resultNoGroupPushdown, expectedResults);
};

let assertResultsMatchWithAndWithoutPushdown = function(
    coll, pipeline, expectedResults, expectedGroupCountInExplain) {
    // Make sure the provided pipeline is eligible for pushdown.
    assertGroupPushdown(coll, pipeline, expectedResults, expectedGroupCountInExplain);

    // Turn sbe off.
    db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true});

    // Sanity check the results when no pushdown happens.
    let resultNoGroupPushdown = coll.aggregate(pipeline).toArray();
    assert.sameMembers(resultNoGroupPushdown, expectedResults);

    // Turn sbe on which will allow $group stages that contain supported accumulators to be pushed
    // down under certain conditions.
    db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: false});

    let resultWithGroupPushdown = coll.aggregate(pipeline).toArray();
    assert.sameMembers(resultNoGroupPushdown, resultWithGroupPushdown);
};

// Try a pipeline with no group stage.
assert.eq(
    coll.aggregate([{$match: {item: "c"}}]).toArray(),
    [{"_id": 5, "item": "c", "price": 5, "quantity": 10, "date": ISODate("2014-02-15T09:05:00Z")}]);

// Run a simple $group with supported $sum accumulator, and check if it gets pushed down.
assertResultsMatchWithAndWithoutPushdown(coll,
                                         [{$group: {_id: "$item", s: {$sum: "$price"}}}],
                                         [{_id: "a", s: 15}, {_id: "b", s: 30}, {_id: "c", s: 5}],
                                         1);

// The subexpression '$not' is not translated to $coerceToolBool and thus is SBE compatible.
assertResultsMatchWithAndWithoutPushdown(coll,
                                         [{$group: {_id: "$item", c: {$sum: {$not: "$price"}}}}],
                                         [{_id: "a", c: 0}, {_id: "b", c: 0}, {_id: "c", c: 0}],
                                         1);

// Two group stages both get pushed down.
assertResultsMatchWithAndWithoutPushdown(
    coll,
    [{$group: {_id: "$item", s: {$sum: "$price"}}}, {$group: {_id: "$quantity", c: {$count: {}}}}],
    [{_id: null, c: 3}],
    2);

// Run a group with an unsupported accumultor and check that it doesn't get pushed down.
assertNoGroupPushdown(coll, [{$group: {_id: "$item", s: {$stdDevSamp: "$quantity"}}}], [
    {"_id": "a", "s": 2.1213203435596424},
    {"_id": "b", "s": 6.363961030678928},
    {"_id": "c", "s": null}
]);

// Run a simple group with $sum and object _id, check if it doesn't get pushed down.
assertNoGroupPushdown(coll,
                      [{$group: {_id: {"i": "$item"}, s: {$sum: "$price"}}}],
                      [{_id: {i: "a"}, s: 15}, {_id: {i: "b"}, s: 30}, {_id: {i: "c"}, s: 5}]);

// Spilling isn't supported yet so $group with 'allowDiskUse' true won't get pushed down.
assertNoGroupPushdown(coll,
                      [{$group: {_id: "$item", s: {$sum: "$price"}}}],
                      [{"_id": "b", "s": 30}, {"_id": "a", "s": 15}, {"_id": "c", "s": 5}],
                      {allowDiskUse: true, cursor: {batchSize: 1}});

// Run a pipeline with match, sort, group to check if the whole pipeline gets pushed down.
assertGroupPushdown(coll,
                    [{$match: {item: "a"}}, {$sort: {price: 1}}, {$group: {_id: "$item"}}],
                    [{"_id": "a"}],
                    1);

// Make sure the DISTINCT_SCAN case where the sort is proided by an index still works and is not
// executed in SBE.
assert.commandWorked(coll.createIndex({item: 1}));
let explain = coll.explain().aggregate([{$sort: {item: 1}}, {$group: {_id: "$item"}}]);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);
assert.commandWorked(coll.dropIndex({item: 1}));

// Time to see if indexes prevent pushdown. Add an index on item, and make sure we don't execute in
// sbe because we won't support $group pushdown until SERVER-58429.
assert.commandWorked(coll.createIndex({item: 1}));
assertNoGroupPushdown(coll,
                      [{$group: {_id: "$item", s: {$sum: "$price"}}}],
                      [{"_id": "b", "s": 30}, {"_id": "a", "s": 15}, {"_id": "c", "s": 5}]);
assert.commandWorked(coll.dropIndex({item: 1}));

// Supported group and then a group with no supported accumulators.
explain = coll.explain().aggregate([
    {$group: {_id: "$item", s: {$sum: "$price"}}},
    {$group: {_id: "$quantity", c: {$stdDevPop: "$price"}}}
]);

assert.neq(null, getAggPlanStage(explain, "GROUP"), explain);
assert(explain.stages[1].hasOwnProperty("$group"));

// Another case of supported group and then a group with no supported accumulators. A boolean
// expression may be translated to an internal expression $coerceToBool which is not supported by
// SBE.
explain = coll.explain().aggregate([
    {$group: {_id: "$item", s: {$sum: "$price"}}},
    {$group: {_id: "$quantity", c: {$sum: {$and: ["$a", true]}}}}
]);

assert.neq(null, getAggPlanStage(explain, "GROUP"), explain);
assert(explain.stages[1].hasOwnProperty("$group"));

// A group with one supported and one unsupported accumulators.
explain = coll.explain().aggregate(
    [{$group: {_id: "$item", s: {$sum: "$price"}, stdev: {$stdDevPop: "$price"}}}]);
assert.eq(null, getAggPlanStage(explain, "GROUP"), explain);
assert(explain.stages[1].hasOwnProperty("$group"));

// $group cannot be pushed down to SBE when there's $match with $or due to an issue with
// subplanning even though $group alone can be pushed down.
const matchWithOr = {
    $match: {$or: [{"item": "a"}, {"price": 10}]}
};
const groupPossiblyPushedDown = {
    $group: {_id: "$item", quantity: {$sum: "$quantity"}}
};
assertNoGroupPushdown(coll,
                      [matchWithOr, groupPossiblyPushedDown],
                      [{_id: "a", quantity: 7}, {_id: "b", quantity: 10}]);

// $bucketAuto/$bucket/$sortByCount are sugared $group stages which are not compatible with SBE.
// TODO SERVER-60300: When we support pushdown of these stages, change these assertions to check
// that they are pushed down.
assertNoGroupPushdown(
    coll,
    [{$bucketAuto: {groupBy: "$price", buckets: 5, output: {quantity: {$sum: "$quantity"}}}}],
    [
        {"_id": {"min": 5, "max": 10}, "quantity": 15},
        {"_id": {"min": 10, "max": 20}, "quantity": 12},
        {"_id": {"min": 20, "max": 20}, "quantity": 1}
    ]);
assertNoGroupPushdown(
    coll,
    [{
        $bucket:
            {groupBy: "$price", boundaries: [1, 10, 50], output: {quantity: {$sum: "$quantity"}}}
    }],
    [{"_id": 1, "quantity": 15}, {"_id": 10, "quantity": 13}]);
assertNoGroupPushdown(
    coll,
    [{$sortByCount: "$item"}],
    [{"_id": "a", "count": 2}, {"_id": "b", "count": 2}, {"_id": "c", "count": 1}]);

// When in a sharded environment or we are spilling $doingMerge is set to true. We should bail out
// and not push down $group stages and the suffix of the pipeline when we encounter a $group stage
// with this flag set.
explain = coll.explain().aggregate([
    {$group: {_id: "$item", s: {$sum: "$price"}}},
    {$group: {_id: "$a", s: {$sum: "$b"}, $doingMerge: true}}
]);
assert.neq(null, getAggPlanStage(explain, "GROUP"), explain);
assert(explain.stages[1].hasOwnProperty("$group"));
})();
