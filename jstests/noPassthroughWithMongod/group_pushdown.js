/**
 * Tests basic functionality of pushing $group into the find layer.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

if (!checkSBEEnabled(db, ["featureFlagSBEGroupPushdown"])) {
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

let assertGroupPushdown = function(
    coll, pipeline, expectedResults, expectedGroupCountInExplain, options = {}) {
    const explain = coll.explain().aggregate(pipeline, options);
    // When $group is pushed down it will never be present as a stage in the 'winningPlan' of
    // $cursor.
    if (expectedGroupCountInExplain > 1) {
        assert.eq(expectedGroupCountInExplain, getAggPlanStages(explain, "GROUP").length, explain);
    } else {
        assert.neq(null, getAggPlanStage(explain, "GROUP"), explain);
    }

    let results = coll.aggregate(pipeline, options).toArray();
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

let assertShardedGroupResultsMatch = function(coll, pipeline, expectedGroupCountInExplain = 1) {
    const originalClassicEngineStatus =
        assert
            .commandWorked(
                db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true}))
            .was;

    const cmd = {
        aggregate: coll.getName(),
        pipeline: pipeline,
        needsMerge: true,
        fromMongos: true,
        cursor: {}
    };

    const classicalRes = coll.runCommand(cmd).cursor.firstBatch;
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: false}));
    const explainCmd = {
        aggregate: coll.getName(),
        pipeline: pipeline,
        needsMerge: true,
        fromMongos: true,
        explain: true,
        cursor: {}
    };
    const explain = coll.runCommand(explainCmd);
    assert.eq(expectedGroupCountInExplain, getAggPlanStages(explain, "GROUP").length, explain);
    const sbeRes = coll.runCommand(cmd).cursor.firstBatch;

    assert.sameMembers(sbeRes, classicalRes);

    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryForceClassicEngine: originalClassicEngineStatus}));
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

// Two group stages both get pushed down and the second $group stage refer to only a top-level field
// which does not exist.
assertResultsMatchWithAndWithoutPushdown(
    coll,
    [{$group: {_id: "$item", s: {$sum: "$price"}}}, {$group: {_id: "$quantity", c: {$count: {}}}}],
    [{_id: null, c: 3}],
    2);

// Two group stages both get pushed down and the second $group stage refers to only existing
// top-level fields of the first $group.
assertResultsMatchWithAndWithoutPushdown(
    coll,
    [
        {$group: {_id: "$item", qsum: {$sum: "$quantity"}, msum: {$sum: "$price"}}},
        {$group: {_id: "$_id", ss: {$sum: {$add: ["$qsum", "$msum"]}}}}
    ],
    [{_id: "a", ss: 22}, {_id: "b", ss: 41}, {_id: "c", ss: 15}],
    2);

// The $group stage refers to the same top-level field twice.
assertResultsMatchWithAndWithoutPushdown(
    coll,
    [{$group: {_id: "$item", ps1: {$sum: "$price"}, ps2: {$sum: "$price"}}}],
    [{_id: "a", ps1: 15, ps2: 15}, {_id: "b", ps1: 30, ps2: 30}, {_id: "c", ps1: 5, ps2: 5}],
    1);

// The $group stage refers to the same top-level field twice and another top-level field.
assertResultsMatchWithAndWithoutPushdown(
    coll,
    [{
        $group:
            {_id: "$item", ps1: {$sum: "$price"}, ps2: {$sum: "$price"}, qs: {$sum: "$quantity"}}
    }],
    [
        {_id: "a", ps1: 15, ps2: 15, qs: 7},
        {_id: "b", ps1: 30, ps2: 30, qs: 11},
        {_id: "c", ps1: 5, ps2: 5, qs: 10}
    ],
    1);

// The $group stage refers to two existing sub-fields.
assertResultsMatchWithAndWithoutPushdown(
    coll,
    [
        {$project: {item: 1, price: 1, quantity: 1, dateParts: {$dateToParts: {date: "$date"}}}},
        {
            $group: {
                _id: "$item",
                hs: {$sum: {$add: ["$dateParts.hour", "$dateParts.hour", "$dateParts.minute"]}}
            }
        },
    ],
    [{"_id": "a", "hs": 39}, {"_id": "b", "hs": 34}, {"_id": "c", "hs": 23}],
    1);

// The $group stage refers to a non-existing sub-field twice.
assertResultsMatchWithAndWithoutPushdown(
    coll,
    [{$group: {_id: "$item", hs: {$sum: {$add: ["$date.hour", "$date.hour"]}}}}],
    [{"_id": "a", "hs": 0}, {"_id": "b", "hs": 0}, {"_id": "c", "hs": 0}],
    1);

// Two group stages both get pushed down and the second $group stage refers to only existing
// top-level fields of the first $group. The field name may be one of "result" / "recordId" /
// "returnKey" / "snapshotId" / "indexId" / "indexKey" / "indexKeyPattern" which are reserved names
// inside the SBE stage builder. These special names must not hide user-defined field names.
[[
    {$group: {_id: "$item", psum: {$sum: "$price"}}},
    {$group: {_id: "$_id", ss: {$sum: {$add: ["$psum", "$psum"]}}}}
],
 [
     {$group: {_id: "$item", result: {$sum: "$price"}}},
     {$group: {_id: "$_id", ss: {$sum: {$add: ["$result", "$result"]}}}}
 ],
 [
     {$group: {_id: "$item", recordId: {$sum: "$price"}}},
     {$group: {_id: "$_id", ss: {$sum: {$add: ["$recordId", "$recordId"]}}}}
 ],
 [
     {$group: {_id: "$item", returnKey: {$sum: "$price"}}},
     {$group: {_id: "$_id", ss: {$sum: {$add: ["$returnKey", "$returnKey"]}}}}
 ],
 [
     {$group: {_id: "$item", snapshotId: {$sum: "$price"}}},
     {$group: {_id: "$_id", ss: {$sum: {$add: ["$snapshotId", "$snapshotId"]}}}}
 ],
 [
     {$group: {_id: "$item", indexId: {$sum: "$price"}}},
     {$group: {_id: "$_id", ss: {$sum: {$add: ["$indexId", "$indexId"]}}}}
 ],
 [
     {$group: {_id: "$item", indexKey: {$sum: "$price"}}},
     {$group: {_id: "$_id", ss: {$sum: {$add: ["$indexKey", "$indexKey"]}}}}
 ],
 [
     {$group: {_id: "$item", indexKeyPattern: {$sum: "$price"}}},
     {$group: {_id: "$_id", ss: {$sum: {$add: ["$indexKeyPattern", "$indexKeyPattern"]}}}}
 ],
].forEach(pipeline =>
              assertResultsMatchWithAndWithoutPushdown(
                  coll, pipeline, [{_id: "a", ss: 30}, {_id: "b", ss: 60}, {_id: "c", ss: 10}], 2));

// The second $group stage refers to both a top-level field and a sub-field twice.
assertResultsMatchWithAndWithoutPushdown(
    coll,
    [
        {$group: {_id: "$item", ps: {$sum: "$price"}}},
        {$group: {_id: "$_id", s1: {$sum: "$ps"}, s2: {$sum: {$add: ["$p.a", "$p.a"]}}}}
    ],
    [
        {"_id": "a", "s1": 15, "s2": 0},
        {"_id": "b", "s1": 30, "s2": 0},
        {"_id": "c", "s1": 5, "s2": 0}
    ],
    2);

// TODO SERVER-59951: Add more test cases that the second $group stage refers to sub-fields when we
// enable $mergeObject or document id expression. As of now we don't have a way to produce valid
// subdocuments from a $group stage.

// Run a group with a supported $stdDevSamp accumultor and check that it gets pushed down.
assertGroupPushdown(coll,
                    [{$group: {_id: "$item", s: {$stdDevSamp: "$quantity"}}}],
                    [
                        {"_id": "a", "s": 2.1213203435596424},
                        {"_id": "b", "s": 6.363961030678928},
                        {"_id": "c", "s": null}
                    ],
                    1);

// Run a simple group with $sum and object _id, check if it doesn't get pushed down.
assertNoGroupPushdown(coll,
                      [{$group: {_id: {"i": "$item"}, s: {$sum: "$price"}}}],
                      [{_id: {i: "a"}, s: 15}, {_id: {i: "b"}, s: 30}, {_id: {i: "c"}, s: 5}]);

// Run a group with spilling on and check that $group is pushed down.
assertGroupPushdown(coll,
                    [{$group: {_id: "$item", s: {$sum: "$price"}}}],
                    [{"_id": "b", "s": 30}, {"_id": "a", "s": 15}, {"_id": "c", "s": 5}],
                    1,
                    {allowDiskUse: true, cursor: {batchSize: 1}});

// Run a pipeline with match, sort, group to check if the whole pipeline gets pushed down.
assertGroupPushdown(coll,
                    [{$match: {item: "a"}}, {$sort: {price: 1}}, {$group: {_id: "$item"}}],
                    [{"_id": "a"}],
                    1);

// Make sure the DISTINCT_SCAN case where the sort is provided by an index still works and is not
// executed in SBE.
assert.commandWorked(coll.createIndex({item: 1}));
let explain = coll.explain().aggregate([{$sort: {item: 1}}, {$group: {_id: "$item"}}]);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);
assert.commandWorked(coll.dropIndex({item: 1}));

// Time to check that indexes don't prevent pushdown.
// The $match stage should trigger usage of indexed plans if there is an index for it. Indexes on
// the fields involved in $group stage should make no difference.
// data schema: {"_id": 1, "item": "a", "price": 10, "quantity": 2, "date": ISODate()}
// The existing index is irrelevant.
assert.commandWorked(coll.createIndex({quantity: 1}));
assertGroupPushdown(coll,
                    [{$match: {price: {$gt: 0}}}, {$group: {_id: "$item", s: {$sum: "$price"}}}],
                    [{"_id": "b", "s": 30}, {"_id": "a", "s": 15}, {"_id": "c", "s": 5}],
                    1 /* expectedGroupCountInExplain */);
// Index on the group by field but the accumulator prevents distinct scan.
assert.commandWorked(coll.createIndex({item: 1}));
assertGroupPushdown(coll,
                    [{$group: {_id: "$item", s: {$sum: "$price"}}}],
                    [{"_id": "b", "s": 30}, {"_id": "a", "s": 15}, {"_id": "c", "s": 5}],
                    1 /* expectedGroupCountInExplain */);
// Multiple relevant indexes.
assert.commandWorked(coll.createIndex({price: 1}));
assertGroupPushdown(coll,
                    [{$match: {price: {$gt: 0}}}, {$group: {_id: "$item", s: {$sum: "$price"}}}],
                    [{"_id": "b", "s": 30}, {"_id": "a", "s": 15}, {"_id": "c", "s": 5}],
                    1 /* expectedGroupCountInExplain */);
// Index on the accumulator field only.
assert.commandWorked(coll.dropIndex({item: 1}));
assertGroupPushdown(coll,
                    [{$group: {_id: "$item", s: {$sum: "$price"}}}],
                    [{"_id": "b", "s": 30}, {"_id": "a", "s": 15}, {"_id": "c", "s": 5}],
                    1 /* expectedGroupCountInExplain */);
assertGroupPushdown(coll,
                    [{$match: {price: {$gt: 0}}}, {$group: {_id: "$item", s: {$sum: "$price"}}}],
                    [{"_id": "b", "s": 30}, {"_id": "a", "s": 15}, {"_id": "c", "s": 5}],
                    1 /* expectedGroupCountInExplain */);
assert.commandWorked(coll.dropIndex({price: 1}));
assert.commandWorked(coll.dropIndex({quantity: 1}));

// Supported group and then a group with unsupported accumulators. JS accumulators are not
// currently pushed down.
explain = coll.explain().aggregate([
    {$group: {_id: "$item", s: {$sum: "$price"}}},
    {
        $group: {
            _id: "$quantity",
            c: {$_internalJsReduce: {data: {k: "$word", v: "$val"}, eval: "null"}}
        }
    }
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
assert.neq(null, getAggPlanStage(explain, "GROUP", true), explain);

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
// A trival $and with only one $or will be optimized away and thus $or will be the top expression.
const matchWithTrivialAndOr = {
    $match: {$and: [{$or: [{"item": "a"}, {"price": 10}]}]}
};
assertNoGroupPushdown(coll,
                      [matchWithTrivialAndOr, groupPossiblyPushedDown],
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

// When at the mongos-side in a sharded environment or we are spilling $doingMerge is set to true.
// We should bail out and not push down $group stages and the suffix of the pipeline when we
// encounter a $group stage with this flag set.
explain = coll.explain().aggregate([
    {$group: {_id: "$item", s: {$sum: "$price"}}},
    {$group: {_id: "$a", s: {$sum: "$b"}, $doingMerge: true}}
]);
assert.neq(null, getAggPlanStage(explain, "GROUP"), explain);
assert(explain.stages[1].hasOwnProperty("$group"));

// In a sharded environment, the mongos splits a $group stage into two different stages. One is a
// merge $group stage at the mongos-side which does the global aggregation and the other is a $group
// stage at the shard-side which does the partial aggregation. The shard-side $group stage is
// requested with 'needsMerge' and 'fromMongos' flags set to true from the mongos, which we should
// verify that is also pushed down and produces the correct results.
explain = coll.runCommand({
    aggregate: coll.getName(),
    explain: true,
    pipeline: [{$group: {_id: "$item"}}],
    needsMerge: true,
    fromMongos: true,
    cursor: {}
});
assert.neq(null, getAggPlanStage(explain, "GROUP"), explain);

// Verifies that a basic sharded $sum accumulator works.
assertShardedGroupResultsMatch(coll, [{$group: {_id: "$item", s: {$sum: "$quantity"}}}]);

// When there's overflow for 'NumberLong', the mongod sends back the partial sum as a doc with
// 'subTotal' and 'subTotalError' fields. So, we need an overflow case to verify such behavior.
const tcoll = db.group_pushdown1;
assert.commandWorked(tcoll.insert([{a: NumberLong("9223372036854775807")}, {a: NumberLong("10")}]));
assertShardedGroupResultsMatch(tcoll, [{$group: {_id: null, s: {$sum: "$a"}}}]);

// Verifies that the shard-side $stdDevPop and $stdDevSamp work.
assertShardedGroupResultsMatch(coll, [{$group: {_id: "$item", s: {$stdDevPop: "$price"}}}]);
assertShardedGroupResultsMatch(coll, [{$group: {_id: "$item", s: {$stdDevSamp: "$price"}}}]);

// Verifies that a sharded $avg works when there's no numeric data.
assertShardedGroupResultsMatch(coll, [{$group: {_id: "$item", a: {$avg: "$missing"}}}]);

// When sum of numeric data is a non-decimal, shard(s) should return data in the form of {subTotal:
// val1, count: val2, subTotalError: val3}.
assertShardedGroupResultsMatch(coll, [{$group: {_id: "$item", a: {$avg: "$quantity"}}}]);

// When sum of numeric data is a decimal, shard(s) should return data in the form of {subTotal:
// val1, count: val2}.
tcoll.drop();
// Prices for group "a" are all decimals.
assert.commandWorked(tcoll.insert(
    [{item: "a", price: NumberDecimal("10.7")}, {item: "a", price: NumberDecimal("20.3")}]));
// Prices for group "b" are one decimal and one non-decimal.
assert.commandWorked(
    tcoll.insert([{item: "b", price: NumberDecimal("3.7")}, {item: "b", price: 2.3}]));
assertShardedGroupResultsMatch(tcoll, [{$group: {_id: "$item", a: {$avg: "$price"}}}]);
})();
