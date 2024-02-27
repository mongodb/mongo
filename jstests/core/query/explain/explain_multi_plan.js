// @tags: [
//   # Cannot implicitly shard accessed collections because of following errmsg: A single
//   # update/delete on a sharded collection must contain an exact match on _id or contain the shard
//   # key.
//   assumes_unsharded_collection,
//   # TODO SERVER-30466
//   does_not_support_causal_consistency,
// ]

import {hasRejectedPlans} from "jstests/libs/analyze_plan.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

/**
 * Tests running explain on a variety of explainable commands (find, update, remove, etc.) when
 * there are multiple plans available. This is a regression test for SERVER-20849 and SERVER-21376.
 */
var coll = db.explainMultiPlan;
coll.drop();

// TODO SERVER-85240: Remove this check when explain is properly implemented for classic runtime
// planning for SBE.
if (FeatureFlagUtil.isPresentAndEnabled(db, "ClassicRuntimePlanningForSbe")) {
    jsTestLog("Skipping test since featureFlagClassicRuntimePlanningForSbe is enabled");
    quit();
}

// Create indices to ensure there are multiple plans available.
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
assert.commandWorked(coll.createIndex({a: -1, b: -1}));

// Insert some data to work with.
var bulk = coll.initializeOrderedBulkOp();
var nDocs = 100;
for (var i = 0; i < nDocs; ++i) {
    bulk.insert({a: i, b: nDocs - i});
}
bulk.execute();

// SERVER-20849: The following commands should not crash the server.
assert.doesNotThrow(function() {
    coll.explain("allPlansExecution").update({a: {$gte: 1}}, {$set: {x: 0}});
});

assert.doesNotThrow(function() {
    coll.explain("allPlansExecution").remove({a: {$gte: 1}});
});

assert.doesNotThrow(function() {
    coll.explain("allPlansExecution").findAndModify({query: {a: {$gte: 1}}, remove: true});
});

assert.doesNotThrow(function() {
    coll.explain("allPlansExecution").findAndModify({query: {a: {$gte: 1}}, update: {y: 1}});
});

assert.doesNotThrow(function() {
    coll.explain("allPlansExecution").find({a: {$gte: 1}}).finish();
});

assert.doesNotThrow(function() {
    coll.explain("allPlansExecution").count({a: {$gte: 1}});
});

assert.doesNotThrow(function() {
    coll.explain("allPlansExecution").distinct("a", {a: {$gte: 1}});
});

var res = coll.explain("queryPlanner").find({a: {$gte: 1}}).finish();
assert.commandWorked(res);
assert(hasRejectedPlans(res));

res = coll.explain("executionStats").find({a: {$gte: 1}}).finish();
assert.commandWorked(res);
assert(hasRejectedPlans(res));
