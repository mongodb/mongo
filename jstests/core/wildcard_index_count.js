// Test that a wildcard index can be used to accelerate count commands, as well as the $count agg
// stage.
//
// The collection cannot be sharded, since the requirement to SHARD_FILTER precludes the planner
// from generating a COUNT_SCAN plan. Further, we do not allow stepdowns, since the code responsible
// for retrying on interrupt is not prepared to handle aggregation explain.
// @tags: [assumes_unsharded_collection, does_not_support_stepdowns]
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const coll = db.wildcard_index_count;
coll.drop();

assert.commandWorked(coll.insert([
    {a: 3},
    {a: null},
    {a: [-1, 0]},
    {a: [4, -3, 5]},
    {},
    {a: {b: 4}},
    {a: []},
    {a: [[], {}]},
    {a: {}},
]));
assert.commandWorked(coll.createIndex({"$**": 1}));

assert.eq(2, coll.count({a: {$gt: 0}}));
assert.eq(2, coll.find({a: {$gt: 0}}).itcount());
assert.eq(2, coll.aggregate([{$match: {a: {$gt: 0}}}, {$count: "count"}]).next().count);

// Verify that this query uses a COUNT_SCAN.
let explain = coll.explain().count({a: {$gt: 0}});
let countScan = getPlanStage(explain.queryPlanner.winningPlan, "COUNT_SCAN");
assert.neq(null, countScan, explain);
assert.eq({$_path: 1, a: 1}, countScan.keyPattern, countScan);

// Query should also COUNT_SCAN when expressed as an aggregation.
explain = coll.explain().aggregate([{$match: {a: {$gt: 0}}}, {$count: "count"}]);
countScan = getAggPlanStage(explain, "COUNT_SCAN");
assert.neq(null, countScan, explain);
assert.eq({$_path: 1, a: 1}, countScan.keyPattern, countScan);

// $count of entire collection does not COUNT_SCAN.
assert.eq(9, coll.find().itcount());
assert.eq(9, coll.aggregate([{$count: "count"}]).next().count);
explain = coll.explain().aggregate([{$count: "count"}]);
countScan = getAggPlanStage(explain, "COUNT_SCAN");
assert.eq(null, countScan, explain);

// When the count consists of multiple intervals, we cannot use COUNT_SCAN.
assert.eq(2, coll.count({a: {$in: [3, 4]}}));
assert.eq(2, coll.find({a: {$in: [3, 4]}}).itcount());
assert.eq(2, coll.aggregate([{$match: {a: {$in: [3, 4]}}}, {$count: "count"}]).next().count);
explain = coll.explain().aggregate([{$match: {a: {$in: [3, 4]}}}, {$count: "count"}]);
countScan = getAggPlanStage(explain, "COUNT_SCAN");
assert.eq(null, countScan, explain);
let ixscan = getAggPlanStage(explain, "IXSCAN");
assert.neq(null, ixscan, explain);
assert.eq({$_path: 1, a: 1}, ixscan.keyPattern, ixscan);

// Count with an equality match on an empty array cannot use COUNT_SCAN.
assert.eq(2, coll.count({a: {$eq: []}}));
assert.eq(2, coll.find({a: {$eq: []}}).itcount());
assert.eq(2, coll.aggregate([{$match: {a: {$eq: []}}}, {$count: "count"}]).next().count);
explain = coll.explain().count({a: {$eq: []}});
countScan = getPlanStage(explain.queryPlanner.winningPlan, "COUNT_SCAN");
assert.eq(null, countScan, explain);
ixscan = getPlanStage(explain.queryPlanner.winningPlan, "IXSCAN");
assert.neq(null, ixscan, explain);
assert.eq({$_path: 1, a: 1}, ixscan.keyPattern, ixscan);

// Count with an equality match on an empty object can use COUNT_SCAN.
assert.eq(2, coll.count({a: {$eq: {}}}));
assert.eq(2, coll.find({a: {$eq: {}}}).itcount());
assert.eq(2, coll.aggregate([{$match: {a: {$eq: {}}}}, {$count: "count"}]).next().count);
explain = coll.explain().count({a: {$eq: {}}});
countScan = getPlanStage(explain.queryPlanner.winningPlan, "COUNT_SCAN");
assert.eq({$_path: 1, a: 1}, countScan.keyPattern, explain);

// Count with equality to a non-empty object cannot use the wildcard index.
assert.eq(1, coll.count({a: {b: 4}}));
assert.eq(1, coll.find({a: {b: 4}}).itcount());
assert.eq(1, coll.aggregate([{$match: {a: {b: 4}}}, {$count: "count"}]).next().count);
explain = coll.explain().count({a: {b: 4}});
assert(isCollscan(db, explain.queryPlanner.winningPlan), explain);

// Count with equality to a non-empty array cannot use the wildcard index.
assert.eq(1, coll.count({a: [-1, 0]}));
assert.eq(1, coll.find({a: [-1, 0]}).itcount());
assert.eq(1, coll.aggregate([{$match: {a: [-1, 0]}}, {$count: "count"}]).next().count);
explain = coll.explain().count({a: [-1, 0]});
assert(isCollscan(db, explain.queryPlanner.winningPlan), explain);
}());
