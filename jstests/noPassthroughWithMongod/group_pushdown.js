/**
 * Tests basic functionality of pushing $group into the find layer.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

if (!checkSBEEnabled(db)) {
    jsTestLog("Skipping test because the sbe group pushdown feature flag is disabled");
    return;
}

// Ensure group pushdown is enabled and capture the original value of
// 'internalQuerySlotBasedExecutionDisableGroupPushdown' to use at the end of the test.
const originalValue = assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQuerySlotBasedExecutionDisableGroupPushdown: false}));
assert(originalValue.hasOwnProperty("was"));
const oldValue = originalValue.was;

const coll = db.group_pushdown;
coll.drop();

const docs = [
    {"_id": 1, "item": "a", "price": 10, "quantity": 2, "date": ISODate("2014-01-01T08:00:00Z")},
    {"_id": 2, "item": "b", "price": 20, "quantity": 1, "date": ISODate("2014-02-03T09:00:00Z")},
    {"_id": 3, "item": "a", "price": 5, "quantity": 5, "date": ISODate("2014-02-03T09:05:00Z")},
    {"_id": 4, "item": "b", "price": 10, "quantity": 10, "date": ISODate("2014-02-15T08:00:00Z")},
    {"_id": 5, "item": "c", "price": 5, "quantity": 10, "date": ISODate("2014-02-15T09:05:00Z")},
];
assert.commandWorked(coll.insert(docs));

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
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"});

    // Sanity check the results when no pushdown happens.
    let resultNoGroupPushdown = coll.aggregate(pipeline).toArray();
    assert.sameMembers(resultNoGroupPushdown, expectedResults);

    // Turn sbe on which will allow $group stages that contain supported accumulators to be pushed
    // down under certain conditions.
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"});

    let resultWithGroupPushdown = coll.aggregate(pipeline).toArray();
    assert.sameMembers(resultNoGroupPushdown, resultWithGroupPushdown);
};

let assertShardedGroupResultsMatch = function(coll, pipeline, expectedGroupCountInExplain = 1) {
    const originalFrameworkControl =
        assert
            .commandWorked(db.adminCommand(
                {setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}))
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
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));
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
        {setParameter: 1, internalQueryFrameworkControl: originalFrameworkControl}));
};

// Try a pipeline with no group stage.
assert.eq(
    coll.aggregate([{$match: {item: "c"}}]).toArray(),
    [{"_id": 5, "item": "c", "price": 5, "quantity": 10, "date": ISODate("2014-02-15T09:05:00Z")}]);

// Run a simple $group with {$sum: 1} accumulator, and check if it gets pushed down.
assertResultsMatchWithAndWithoutPushdown(
    coll,
    [{$group: {_id: "$item", c: {$sum: NumberInt(1)}}}],
    [{_id: "a", c: NumberInt(2)}, {_id: "b", c: NumberInt(2)}, {_id: "c", c: NumberInt(1)}],
    1);

assertResultsMatchWithAndWithoutPushdown(
    coll,
    [{$group: {_id: "$item", c: {$sum: NumberLong(1)}}}],
    [{_id: "a", c: NumberLong(2)}, {_id: "b", c: NumberLong(2)}, {_id: "c", c: NumberLong(1)}],
    1);

assertResultsMatchWithAndWithoutPushdown(coll,
                                         [{$group: {_id: "$item", c: {$sum: 1}}}],
                                         [{_id: "a", c: 2}, {_id: "b", c: 2}, {_id: "c", c: 1}],
                                         1);

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

// The second $group stage refers to both a top-level field and a sub-field twice which does not
// exist.
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

// The second $group stage refers to a sub-field which does exist.
assertResultsMatchWithAndWithoutPushdown(
    coll,
    [
        {$group: {_id: {i: "$item", p: {$divide: ["$price", 5]}}}},
        {$group: {_id: "$_id.p", s: {$sum: 1}}}
    ],
    [{"_id": 1, "s": 2}, {"_id": 2, "s": 2}, {"_id": 4, "s": 1}],
    2);

// Verifies that an optimized expression can be pushed down.
assertResultsMatchWithAndWithoutPushdown(
    coll,
    // {"$ifNull": [1, 2]} will be optimized into just the constant 1.
    [{$group: {_id: {"$ifNull": [1, 2]}, o: {$min: "$quantity"}}}],
    [{"_id": 1, o: 1}],
    1);

// Run a group with a supported $stdDevSamp accumultor and check that it gets pushed down.
assertGroupPushdown(coll,
                    [{$group: {_id: "$item", s: {$stdDevSamp: "$quantity"}}}],
                    [
                        {"_id": "a", "s": 2.1213203435596424},
                        {"_id": "b", "s": 6.363961030678928},
                        {"_id": "c", "s": null}
                    ],
                    1);

// Run a simple group with $sum and object _id, check if it gets pushed down.
assertGroupPushdown(coll,
                    [{$group: {_id: {"i": "$item"}, s: {$sum: "$price"}}}],
                    [{_id: {i: "a"}, s: 15}, {_id: {i: "b"}, s: 30}, {_id: {i: "c"}, s: 5}]);

// Test that we can push down a $group and a projection.
assertGroupPushdown(
    coll,
    [{$project: {_id: 0, item: 1, price: 1}}, {$group: {_id: {i: "$item"}, s: {$sum: "$price"}}}],
    [{_id: {i: "a"}, s: 15}, {_id: {i: "b"}, s: 30}, {_id: {i: "c"}, s: 5}]);

// Test that the results are as expected if the projection comes first and removes a field that the
// $group stage needs.
assertGroupPushdown(
    coll,
    [{$project: {_id: 0, item: 1}}, {$group: {_id: {i: "$item"}, s: {$sum: "$price"}}}],
    [{_id: {i: "a"}, s: 0}, {_id: {i: "b"}, s: 0}, {_id: {i: "c"}, s: 0}]);

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

// $group can be pushed down to SBE when subplanning is involved. Note that the top $or expression
// triggers subplanning.
(function() {
// Use another collection to not interfere with other test cases even when a test case fails since
// we create indexes to verify group pushdown when subplanning is involed.
const coll = db.group_pushdown_subplanning;
coll.drop();

assert.commandWorked(coll.insert(docs));

const verifyGroupPushdownWhenSubplanning = () => {
    const matchWithOr = {$match: {$or: [{"item": "a"}, {"price": 10}]}};
    const groupPushedDown = {$group: {_id: "$item", quantity: {$sum: "$quantity"}}};
    assertResultsMatchWithAndWithoutPushdown(coll,
                                             [matchWithOr, groupPushedDown],
                                             [{_id: "a", quantity: 7}, {_id: "b", quantity: 10}],
                                             1);
    // A trival $and with only one $or will be optimized away and thus $or will be the top
    // expression.
    const matchWithTrivialAndOr = {$match: {$and: [{$or: [{"item": "a"}, {"price": 10}]}]}};
    assertResultsMatchWithAndWithoutPushdown(coll,
                                             [matchWithTrivialAndOr, groupPushedDown],
                                             [{_id: "a", quantity: 7}, {_id: "b", quantity: 10}],
                                             1);
};

// Verify that $group can be pushed down when subplanning is involved. With this test case,
// subplanning code path is involved but "Subplanning" does not actually happen and instead,
// it falls back to planning a whole query.
verifyGroupPushdownWhenSubplanning();

// Create indexes on 'item' and 'price' fields to cover all sub-expressions of $match.
coll.createIndex({item: 1});
coll.createIndex({price: 1});

// Verify that $group can be pushed down when there are indexes that cover all sub-expressions
// and "Subplanning" actually happens.
verifyGroupPushdownWhenSubplanning();
}());

// $bucketAuto is a group-like stage that is not compatible with SBE HashAggStage.
// TODO SERVER-62401: Supporting a $bucketAuto will require a range-based group-aggregate
// implementation that will chose _ids based on the collection of values rather than a hash-based
// group-aggregate that requires _id to be computable by looking just at the current document.

assertNoGroupPushdown(
    coll,
    [{$bucketAuto: {groupBy: "$price", buckets: 5, output: {quantity: {$sum: "$quantity"}}}}],
    [
        {"_id": {"min": 5, "max": 10}, "quantity": 15},
        {"_id": {"min": 10, "max": 20}, "quantity": 12},
        {"_id": {"min": 20, "max": 20}, "quantity": 1}
    ]);

// Verify that $bucket is pushed down to SBE and returns correct results.
assertResultsMatchWithAndWithoutPushdown(
    coll,
    [{
        $bucket:
            {groupBy: "$price", boundaries: [1, 10, 50], output: {quantity: {$sum: "$quantity"}}}
    }],
    [{"_id": 1, "quantity": 15}, {"_id": 10, "quantity": 13}]);

assertResultsMatchWithAndWithoutPushdown(
    coll,
    [{
        $bucket: {
            groupBy: "$price",
            boundaries: [1, 50],
            output: {count: {$count: {}}, quantity: {$sum: "$quantity"}}
        }
    }],
    [{"_id": 1, "count": 5, "quantity": 28}]);

// Verify that $sortByCount is pushed down to SBE and returns correct results.
assertResultsMatchWithAndWithoutPushdown(
    coll,
    [{$sortByCount: "$item"}],
    [{"_id": "a", "count": 2}, {"_id": "b", "count": 2}, {"_id": "c", "count": 1}]);

assertResultsMatchWithAndWithoutPushdown(
    coll,
    [{$sortByCount: {$cond: [{$eq: ["$item", {$const: "a"}]}, "$price", "$quantity"]}}],
    [{_id: 10, count: 3}, {_id: 1, count: 1}, {_id: 5, count: 1}]);

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

// Verifies that a sharded count-like accumulator works
assertShardedGroupResultsMatch(coll, [{$group: {_id: "$item", s: {$sum: NumberInt(1)}}}]);
assertShardedGroupResultsMatch(coll, [{$group: {_id: "$item", s: {$sum: NumberLong(1)}}}]);
assertShardedGroupResultsMatch(coll, [{$group: {_id: "$item", s: {$sum: 1}}}]);
assertShardedGroupResultsMatch(coll, [{$group: {_id: "$item", s: {$count: {}}}}]);

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

// Verify that $group pushdown can be disabled with the
// 'internalQuerySlotBasedExecutionDisableGroupPushdown' flag.
const basicGroup = [{$group: {_id: "$item", out: {$sum: 1}}}];
const basicGroupResults = [{_id: "a", out: 2}, {_id: "b", out: 2}, {_id: "c", out: 1}];

// $group pushdown should work as expected before setting
// 'internalQuerySlotBasedExecutionDisableGroupPushdown' to true.
assertGroupPushdown(coll,
                    basicGroup,
                    basicGroupResults,
                    /* expectedGroupCountInExplain */ 1);

// Turn group pushdown off.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQuerySlotBasedExecutionDisableGroupPushdown: true}));
assertNoGroupPushdown(coll, basicGroup, basicGroupResults);

// Reset 'internalQuerySlotBasedExecutionDisableGroupPushdown' to its original value.
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQuerySlotBasedExecutionDisableGroupPushdown: oldValue}));

(function testConstNothingForIdMappedToNull() {
    // Prepare a collection.
    const coll = db.nothing_id;
    coll.drop();
    coll.insert({_id: 0});

    // $$REMOVE produce Nothing constant and it should be converted to Null. Without an accumulator
    // $group is not pushed down and we need an accumulator.
    assert.eq(
        coll.aggregate([{$group: {_id: "$$REMOVE", o: {$first: "$non_existent_field"}}}]).toArray(),
        [{_id: null, o: null}]);
})();
})();
