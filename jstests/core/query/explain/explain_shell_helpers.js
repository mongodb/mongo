/**
 * Cannot implicitly shard accessed collections because the explain output from a mongod when run
 * against a sharded collection is wrapped in a "shards" object with keys for each shard.
 *
 * @tags: [
 *   # The 'totalDocsExamined' values reported by explain queries can be higher than expected by
 *   # this test if the balancer moves chunks around while the explain queries are running.
 *   assumes_balancer_off,
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 *   requires_fastcount,
 *   # TODO SERVER-30466
 *   does_not_support_causal_consistency,
 *   # Sanitizers slow down the server and can break reasonable timeouts set on queries. This test
 *   # is for shell helpers, so the sanitizers are unnecessary anyway.
 *   incompatible_aubsan,
 * ]
 */

// Tests for the .explain() shell helper, which provides syntactic sugar for the explain command.
// Include helpers for analyzing explain output.
import {
    getPlanStage,
    getSingleNodeExplain,
    getWinningPlanFromExplain,
    isIxscan,
    planHasStage,
} from "jstests/libs/query/analyze_plan.js";

let t = db.jstests_explain_helpers;
t.drop();

let explain;
var stage;

t.createIndex({a: 1});
for (let i = 0; i < 10; i++) {
    t.insert({_id: i, a: i, b: 1});
}

//
// Basic .find()
//

// No verbosity specified means that we should use "queryPlanner" verbosity.
explain = t.explain().find().finish();
assert.commandWorked(explain);
assert("queryPlanner" in explain);
assert(!("executionStats" in explain));

// .explain() can also come after .find().
explain = t.find().explain();
assert.commandWorked(explain);
assert("queryPlanner" in explain);
assert(!("executionStats" in explain));

// .explain(true) means get execution stats for all candidate plans.
explain = t.explain(true).find().finish();
assert.commandWorked(explain);
assert("queryPlanner" in explain);
assert("executionStats" in explain);
assert("allPlansExecution" in explain.executionStats);

// .explain(true) after .find().
explain = t.find().explain(true);
assert.commandWorked(explain);
assert("queryPlanner" in explain);
assert("executionStats" in explain);
assert("allPlansExecution" in explain.executionStats);

//
// Test verbosity specifiers.
//

// "queryPlanner"
explain = t.explain("queryPlanner").find().finish();
assert.commandWorked(explain);
assert("queryPlanner" in explain);
assert(!("executionStats" in explain));
explain = t.find().explain("queryPlanner");
assert.commandWorked(explain);
assert("queryPlanner" in explain);
assert(!("executionStats" in explain));

// "executionStats"
explain = t.explain("executionStats").find().finish();
assert.commandWorked(explain);
assert("queryPlanner" in explain);
assert("executionStats" in explain);
assert(!("allPlansExecution" in explain.executionStats));
explain = t.find().explain("executionStats");
assert.commandWorked(explain);
assert("queryPlanner" in explain);
assert("executionStats" in explain);
assert(!("allPlansExecution" in explain.executionStats));

// "allPlansExecution"
explain = t.explain("allPlansExecution").find().finish();
assert.commandWorked(explain);
assert("queryPlanner" in explain);
assert("executionStats" in explain);
assert("allPlansExecution" in explain.executionStats);
explain = t.find().explain("allPlansExecution");
assert.commandWorked(explain);
assert("queryPlanner" in explain);
assert("executionStats" in explain);
assert("allPlansExecution" in explain.executionStats);

//
// Tests for DBExplainQuery helpers.
//

// .limit()
explain = t.explain().find().limit(3).finish();
assert.commandWorked(explain);
explain = t.find().limit(3).explain();
assert.commandWorked(explain);

// .batchSize()
explain = t.explain().find().batchSize(3).finish();
assert.commandWorked(explain);
explain = t.find().batchSize(3).explain();
assert.commandWorked(explain);

// .addOption()
explain = t.explain().find().addOption(DBQuery.Option.noTimeout).finish();
assert.commandWorked(explain);
explain = t.find().batchSize(DBQuery.Option.noTimeout).explain();
assert.commandWorked(explain);

// .skip()
explain = t.explain().find().skip(3).finish();
assert.commandWorked(explain);
explain = t.find().skip(3).explain();
assert.commandWorked(explain);

// .sort()
explain = t.explain().find().sort({b: -1}).finish();
assert.commandWorked(explain);
assert(planHasStage(db, getWinningPlanFromExplain(explain), "SORT"));
explain = t.find().sort({b: -1}).explain();
assert.commandWorked(explain);
assert(planHasStage(db, getWinningPlanFromExplain(explain), "SORT"));

// .hint()
explain = t.explain().find().hint({a: 1}).finish();
assert.commandWorked(explain);
assert(isIxscan(db, getWinningPlanFromExplain(explain)));
explain = t.explain().find().hint("a_1").finish();
assert.commandWorked(explain);
assert(isIxscan(db, getWinningPlanFromExplain(explain)));
explain = t.find().hint({a: 1}).explain();
assert.commandWorked(explain);
assert(isIxscan(db, getWinningPlanFromExplain(explain)));
explain = t.find().hint("a_1").explain();
assert.commandWorked(explain);
assert(isIxscan(db, getWinningPlanFromExplain(explain)));

// .min()
explain = t.explain().find().min({a: 1}).hint({a: 1}).finish();
assert.commandWorked(explain);
assert(isIxscan(db, getWinningPlanFromExplain(explain)));
explain = t.find().min({a: 1}).hint({a: 1}).explain();
assert.commandWorked(explain);
assert(isIxscan(db, getWinningPlanFromExplain(explain)));

// .max()
explain = t.explain().find().max({a: 1}).hint({a: 1}).finish();
assert.commandWorked(explain);
assert(isIxscan(db, getWinningPlanFromExplain(explain)));
explain = t.find().max({a: 1}).hint({a: 1}).explain();
assert.commandWorked(explain);
assert(isIxscan(db, getWinningPlanFromExplain(explain)));

// .allowDiskUse()
explain = t.explain().find().allowDiskUse().finish();
assert.commandWorked(explain);
explain = t.find().allowDiskUse().explain();
assert.commandWorked(explain);

// .showDiskLoc()
explain = t.explain().find().showDiskLoc().finish();
assert.commandWorked(explain);
explain = t.find().showDiskLoc().explain();
assert.commandWorked(explain);

// .maxTimeMS()
// Provide longer maxTime when the test runs in suites which can affect query execution time.
// In slow suites set the maxTime to at least 5s to account for the suites' overhead.
const numConn = db.serverStatus().connections.current;
const maxTimeMS = Math.max(500 * numConn, 5000);
explain = t.explain().find().maxTimeMS(maxTimeMS).finish();
assert.commandWorked(explain);
explain = t.find().maxTimeMS(maxTimeMS).explain();
assert.commandWorked(explain);

// .readPref()
explain = t.explain().find().readPref("secondaryPreferred").finish();
assert.commandWorked(explain);
explain = t.find().readPref("secondaryPreferred").explain();
assert.commandWorked(explain);

// .comment()
explain = t.explain().find().comment("test .comment").finish();
assert.commandWorked(explain);
explain = t.find().comment("test .comment").explain();
assert.commandWorked(explain);

// .next()
explain = t.explain().find().next();
assert.commandWorked(explain);
assert("queryPlanner" in explain);

// .hasNext()
let explainQuery = t.explain().find();
assert(explainQuery.hasNext());
assert.commandWorked(explainQuery.next());
assert(!explainQuery.hasNext());

// .forEach()
let results = [];
t.explain()
    .find()
    .forEach(function (res) {
        results.push(res);
    });
assert.eq(1, results.length);
assert.commandWorked(results[0]);

//
// .aggregate()
// $group stage might be lowered into SBE which would remove the pipeline. As the goal here is to
// test the explain helpers for the case when pipeline is used, we suppress the potential
// optimizations with '$_internalInhibitOptimization' stage.
//

explain = t.explain().aggregate([{$_internalInhibitOptimization: {}}, {$match: {a: 3}}, {$group: {_id: null}}]);
assert.commandWorked(explain);
explain = getSingleNodeExplain(explain);
assert.eq(4, explain.stages.length);
assert("queryPlanner" in explain.stages[0].$cursor);

// Legacy varargs format.
explain = t.explain().aggregate({$_internalInhibitOptimization: {}}, {$group: {_id: null}});
assert.commandWorked(explain);
explain = getSingleNodeExplain(explain);
assert.eq(3, explain.stages.length);
assert("queryPlanner" in explain.stages[0].$cursor);

explain = t.explain().aggregate({$_internalInhibitOptimization: {}}, {$project: {a: 3}}, {$group: {_id: null}});
assert.commandWorked(explain);
explain = getSingleNodeExplain(explain);
assert.eq(4, explain.stages.length);
assert("queryPlanner" in explain.stages[0].$cursor);

// Options already provided.
explain = t
    .explain()
    .aggregate([{$_internalInhibitOptimization: {}}, {$match: {a: 3}}, {$group: {_id: null}}], {allowDiskUse: true});
assert.commandWorked(explain);
explain = getSingleNodeExplain(explain);
assert.eq(4, explain.stages.length);
assert("queryPlanner" in explain.stages[0].$cursor);

//
// .count()
//

// Basic count.
explain = t.explain().count();
assert.commandWorked(explain);
explain = getSingleNodeExplain(explain);
assert(planHasStage(db, getWinningPlanFromExplain(explain), "RECORD_STORE_FAST_COUNT"));

// Tests for applySkipLimit argument to .count. When we don't apply the skip, we
// count one result. When we do apply the skip we count zero.
explain = t.explain("executionStats").find({a: 3}).skip(1).count(false);
stage = explain.executionStats.executionStages;
if ("SINGLE_SHARD" === stage.stage) {
    stage = stage.shards[0].executionStages;
}
assert.eq(1, stage.nCounted);
explain = t.explain("executionStats").find({a: 3}).skip(1).count(true);
stage = explain.executionStats.executionStages;
if ("SINGLE_SHARD" === stage.stage) {
    stage = stage.shards[0].executionStages;
}
assert.eq(0, stage.nCounted);

// Count with hint.
explain = t.explain().find({a: 3}).hint({a: 1}).count();
assert.commandWorked(explain);
explain = getSingleNodeExplain(explain);
assert(planHasStage(db, getWinningPlanFromExplain(explain), "COUNT"));
assert(planHasStage(db, getWinningPlanFromExplain(explain), "COUNT_SCAN"));

// Explainable count with hint.
assert.commandWorked(t.createIndex({c: 1}, {sparse: true}));
explain = t.explain().count({c: {$exists: false}}, {hint: "c_1"});
assert.commandWorked(explain);
explain = getSingleNodeExplain(explain);
assert(planHasStage(db, getWinningPlanFromExplain(explain), "IXSCAN"));
assert.eq(getPlanStage(getWinningPlanFromExplain(explain), "IXSCAN").indexName, "c_1");
assert.commandWorked(t.dropIndex({c: 1}));

//
// .distinct()
//

explain = t.explain().distinct("_id");
assert.commandWorked(explain);
assert(planHasStage(db, getWinningPlanFromExplain(explain), "PROJECTION_COVERED"));
assert(planHasStage(db, getWinningPlanFromExplain(explain), "DISTINCT_SCAN"));

explain = t.explain().distinct("a");
assert.commandWorked(explain);
assert(planHasStage(db, getWinningPlanFromExplain(explain), "PROJECTION_COVERED"));
assert(planHasStage(db, getWinningPlanFromExplain(explain), "DISTINCT_SCAN"));

explain = t.explain().distinct("b");
assert.commandWorked(explain);
assert(planHasStage(db, getWinningPlanFromExplain(explain), "COLLSCAN"));

//
// .remove()
//

// Check that there is one matching document.
assert.eq(1, t.find({a: 3}).itcount());

// Explain a single-document delete.
explain = t.explain("executionStats").remove({a: 3}, true);
assert.commandWorked(explain);
assert.eq(1, explain.executionStats.totalDocsExamined);

// Document should not have been deleted.
assert.eq(1, t.find({a: 3}).itcount());

// Explain a single-document delete with the new syntax.
explain = t.explain("executionStats").remove({a: 3}, {justOne: true});
assert.commandWorked(explain);
assert.eq(1, explain.executionStats.totalDocsExamined);

// Document should not have been deleted.
assert.eq(1, t.find({a: 3}).itcount());

// Explain a multi-document delete.
explain = t.explain("executionStats").remove({a: {$lte: 2}});
assert.commandWorked(explain);
assert.eq(3, explain.executionStats.totalDocsExamined);

// All 10 docs in the collection should still be present.
assert.eq(10, t.count());

//
// .update()
//

// Basic update.
explain = t.explain("executionStats").update({a: 3}, {$set: {b: 3}});
assert.commandWorked(explain);
assert.eq(1, explain.executionStats.totalDocsExamined);

// Document should not have been updated.
assert.eq(1, t.findOne({a: 3})["b"]);

// Update with upsert flag set that should do an insert.
explain = t.explain("executionStats").update({a: 15}, {$set: {b: 3}}, true);
assert.commandWorked(explain);
stage = explain.executionStats.executionStages;
if ("SHARD_WRITE" === stage.stage) {
    stage = stage.shards[0].executionStages;
}
assert.eq(stage.stage, "UPDATE");
assert(stage.nWouldUpsert == 1);

// Make sure that the insert didn't actually happen.
assert.eq(10, t.count());

// Use the {upsert: true} syntax.
explain = t.explain("executionStats").update({a: 15}, {$set: {b: 3}}, {upsert: true});
assert.commandWorked(explain);
var stage = explain.executionStats.executionStages;
if ("SHARD_WRITE" === stage.stage) {
    stage = stage.shards[0].executionStages;
}
assert.eq(stage.stage, "UPDATE");
assert(stage.nWouldUpsert == 1);
assert.eq(0, stage.nMatched);

// Make sure that the insert didn't actually happen.
assert.eq(10, t.count());

// Update with multi-update flag set.
explain = t.explain("executionStats").update({a: {$lte: 2}}, {$set: {b: 3}}, false, true);
assert.commandWorked(explain);
var stage = explain.executionStats.executionStages;
if ("SHARD_WRITE" === stage.stage) {
    stage = stage.shards[0].executionStages;
}
assert.eq(stage.stage, "UPDATE");
assert(stage.nWouldUpsert == 0);
assert.eq(3, stage.nMatched);
assert.eq(3, stage.nWouldModify);

// Use the {multi: true} syntax.
explain = t.explain("executionStats").update({a: {$lte: 2}}, {$set: {b: 3}}, {multi: true});
assert.commandWorked(explain);
var stage = explain.executionStats.executionStages;
if ("SHARD_WRITE" === stage.stage) {
    stage = stage.shards[0].executionStages;
}
assert.eq(stage.stage, "UPDATE");
assert(stage.nWouldUpsert == 0);
assert.eq(3, stage.nMatched);
assert.eq(3, stage.nWouldModify);

//
// .findAndModify()
//

// Basic findAndModify with update.
explain = t.explain("executionStats").findAndModify({query: {a: 3}, update: {$set: {b: 3}}});
assert.commandWorked(explain);
assert.eq(1, explain.executionStats.totalDocsExamined);

// Document should not have been updated.
assert.eq(1, t.findOne({a: 3})["b"]);

// Basic findAndModify with delete.
explain = t.explain("executionStats").findAndModify({query: {a: 3}, remove: true});
assert.commandWorked(explain);
assert.eq(1, explain.executionStats.totalDocsExamined);

// Delete shouldn't have happened.
assert.eq(10, t.count());

// findAndModify with upsert flag set that should do an insert.
explain = t.explain("executionStats").findAndModify({query: {a: 15}, update: {$set: {b: 3}}, upsert: true});
assert.commandWorked(explain);
stage = explain.executionStats.executionStages;
if ("SINGLE_SHARD" === stage.stage) {
    stage = stage.shards[0].executionStages;
}
assert.eq(stage.stage, "UPDATE");
assert(stage.nWouldUpsert == 1);

// Make sure that the insert didn't actually happen.
assert.eq(10, t.count());

//
// Error cases.
//

// Can't explain an update without a query.
assert.throws(function () {
    t.explain().update();
});

// Can't explain an update without mods.
assert.throws(function () {
    t.explain().update({a: 3});
});

// Can't add fourth arg when using document-style specification of update options.
assert.throws(function () {
    t.explain().update({a: 3}, {$set: {b: 4}}, {multi: true}, true);
});

// Can't specify both remove and update in a findAndModify
assert.throws(function () {
    t.explain().findAndModify({remove: true, update: {$set: {b: 3}}});
});
