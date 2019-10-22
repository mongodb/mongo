// From the work done for SERVER-22093, an aggregation pipeline that does not require any fields
// from the input documents will tell the query planner to use a count scan, which is faster than an
// index scan. In this test file, we check this behavior through explain().
//
// Cannot implicitly shard accessed collections because the explain output from a mongod when run
// against a sharded collection is wrapped in a "shards" object with keys for each shard.
//
// This test assumes that an initial $match will be absorbed by the query system, which will not
// happen if the $match is wrapped within a $facet stage.
// @tags: [do_not_wrap_aggregations_in_facets,assumes_unsharded_collection]
load('jstests/libs/analyze_plan.js');

(function() {
"use strict";

var coll = db.countscan;
coll.drop();

for (var i = 0; i < 3; i++) {
    for (var j = 0; j < 10; j += 2) {
        coll.insert({foo: i, bar: j});
    }
}

coll.ensureIndex({foo: 1});

var simpleGroup = coll.aggregate([{$group: {_id: null, count: {$sum: 1}}}]).toArray();

assert.eq(simpleGroup.length, 1);
assert.eq(simpleGroup[0]["count"], 15);

var explained =
    coll.explain().aggregate([{$match: {foo: {$gt: 0}}}, {$group: {_id: null, count: {$sum: 1}}}]);

assert(planHasStage(db, explained.stages[0].$cursor.queryPlanner.winningPlan, "COUNT_SCAN"));

explained = coll.explain().aggregate([
    {$match: {foo: {$gt: 0}}},
    {$project: {_id: 0, a: {$literal: null}}},
    {$group: {_id: null, count: {$sum: 1}}}
]);

assert(planHasStage(db, explained.stages[0].$cursor.queryPlanner.winningPlan, "COUNT_SCAN"));

// Make sure a $count stage can use the COUNT_SCAN optimization.
explained = coll.explain().aggregate([{$match: {foo: {$gt: 0}}}, {$count: "count"}]);
assert(planHasStage(db, explained.stages[0].$cursor.queryPlanner.winningPlan, "COUNT_SCAN"));

// A $match that is not a single range cannot use the COUNT_SCAN optimization.
explained = coll.explain().aggregate([{$match: {foo: {$in: [0, 1]}}}, {$count: "count"}]);
assert(!planHasStage(db, explained.stages[0].$cursor.queryPlanner.winningPlan, "COUNT_SCAN"));

// Test that COUNT_SCAN can be used when there is a $sort.
explained = coll.explain().aggregate([{$sort: {foo: 1}}, {$count: "count"}]);
assert(aggPlanHasStage(explained, "COUNT_SCAN"), explained);

// Test that a forward COUNT_SCAN plan is chosen even when there is a $sort in the direction
// opposite that of the index.
explained = coll.explain().aggregate([{$sort: {foo: -1}}, {$count: "count"}]);
let countScan = getAggPlanStage(explained, "COUNT_SCAN");
assert.neq(null, countScan, explained);
assert.eq({foo: MinKey}, countScan.indexBounds.startKey, explained);
assert.eq(true, countScan.indexBounds.startKeyInclusive, explained);
assert.eq({foo: MaxKey}, countScan.indexBounds.endKey, explained);
assert.eq(true, countScan.indexBounds.endKeyInclusive, explained);

// Test that the inclusivity/exclusivity of the index bounds for COUNT_SCAN are correct when there
// is a $sort in the opposite direction of the index.
explained = coll.explain().aggregate(
    [{$match: {foo: {$gte: 0, $lt: 10}}}, {$sort: {foo: -1}}, {$count: "count"}]);
countScan = getAggPlanStage(explained, "COUNT_SCAN");
assert.neq(null, countScan, explained);
assert.eq({foo: 0}, countScan.indexBounds.startKey, explained);
assert.eq(true, countScan.indexBounds.startKeyInclusive, explained);
assert.eq({foo: 10}, countScan.indexBounds.endKey, explained);
assert.eq(false, countScan.indexBounds.endKeyInclusive, explained);
}());
