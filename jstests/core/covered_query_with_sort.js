// Test covered query with sort stage between index scan and projection.
//
// @tags: [
//   # Cannot implicitly shard accessed collections because queries on a sharded collection are not
//   # able to be covered when they aren't on the shard key since the document needs to be fetched
//   # in order to apply the SHARDING_FILTER stage.
//   assumes_unsharded_collection,
// ]

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For 'isIndexOnly', 'getPlanStage' and 'getWinningPlan'.

const coll = db.covered_query_with_sort;
coll.drop();

assert.commandWorked(coll.insert([
    {y: 0, x: 0},
    {y: 0, x: 1},
    {y: 1, x: -1},
]));
assert.commandWorked(coll.createIndex({y: 1, x: 1}));

function buildQuery() {
    return coll.find({y: {$gte: 0}, x: {$gte: 0}}, {_id: 0, y: 1, x: 1}).sort({x: -1}).limit(2);
}

// Ensure that query is covered.
const plan = buildQuery().explain();
assert(isIndexOnly(db, getWinningPlan(plan.queryPlanner)), plan);

// Ensure that query plan has shape IXSCAN => SORT => PROJECTION_COVERED.
const projectionCoveredStage = getPlanStage(plan, "PROJECTION_COVERED");
assert.neq(projectionCoveredStage, null, plan);
const sortStage = getPlanStage(projectionCoveredStage, "SORT");
assert.neq(sortStage, null, plan);
const ixScanStage = getPlanStage(projectionCoveredStage, "IXSCAN");
assert.neq(ixScanStage, null, plan);

const results = buildQuery().toArray();
assert.eq(results, [{y: 0, x: 1}, {y: 0, x: 0}], results);
}());
