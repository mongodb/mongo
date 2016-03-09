// Tests for the .explain() shell helper, which provides syntactic sugar for the explain command.

var t = db.jstests_explain_helpers;
t.drop();

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");

var explain;
var stage;

t.ensureIndex({a: 1});
for (var i = 0; i < 10; i++) {
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
assert(planHasStage(explain.queryPlanner.winningPlan, "SORT"));
explain = t.find().sort({b: -1}).explain();
assert.commandWorked(explain);
assert(planHasStage(explain.queryPlanner.winningPlan, "SORT"));

// .hint()
explain = t.explain().find().hint({a: 1}).finish();
assert.commandWorked(explain);
assert(isIxscan(explain.queryPlanner.winningPlan));
explain = t.explain().find().hint("a_1").finish();
assert.commandWorked(explain);
assert(isIxscan(explain.queryPlanner.winningPlan));
explain = t.find().hint({a: 1}).explain();
assert.commandWorked(explain);
assert(isIxscan(explain.queryPlanner.winningPlan));
explain = t.find().hint("a_1").explain();
assert.commandWorked(explain);
assert(isIxscan(explain.queryPlanner.winningPlan));

// .min()
explain = t.explain().find().min({a: 1}).finish();
assert.commandWorked(explain);
assert(isIxscan(explain.queryPlanner.winningPlan));
explain = t.find().min({a: 1}).explain();
assert.commandWorked(explain);
assert(isIxscan(explain.queryPlanner.winningPlan));

// .max()
explain = t.explain().find().max({a: 1}).finish();
assert.commandWorked(explain);
assert(isIxscan(explain.queryPlanner.winningPlan));
explain = t.find().max({a: 1}).explain();
assert.commandWorked(explain);
assert(isIxscan(explain.queryPlanner.winningPlan));

// .showDiskLoc()
explain = t.explain().find().showDiskLoc().finish();
assert.commandWorked(explain);
explain = t.find().showDiskLoc().explain();
assert.commandWorked(explain);

// .maxTimeMS()
explain = t.explain().find().maxTimeMS(200).finish();
assert.commandWorked(explain);
explain = t.find().maxTimeMS(200).explain();
assert.commandWorked(explain);

// .readPref()
explain = t.explain().find().readPref("secondary").finish();
assert.commandWorked(explain);
explain = t.find().readPref("secondary").explain();
assert.commandWorked(explain);

// .comment()
explain = t.explain().find().comment("test .comment").finish();
assert.commandWorked(explain);
explain = t.find().comment("test .comment").explain();
assert.commandWorked(explain);

// .snapshot()
explain = t.explain().find().snapshot().finish();
assert.commandWorked(explain);
explain = t.find().snapshot().explain();
assert.commandWorked(explain);

// .next()
explain = t.explain().find().next();
assert.commandWorked(explain);
assert("queryPlanner" in explain);

// .hasNext()
var explainQuery = t.explain().find();
assert(explainQuery.hasNext());
assert.commandWorked(explainQuery.next());
assert(!explainQuery.hasNext());

// .forEach()
var results = [];
t.explain().find().forEach(function(res) {
    results.push(res);
});
assert.eq(1, results.length);
assert.commandWorked(results[0]);

//
// .aggregate()
//

explain = t.explain().aggregate([{$match: {a: 3}}]);
assert.commandWorked(explain);
assert.eq(1, explain.stages.length);
assert("queryPlanner" in explain.stages[0].$cursor);

// Legacy varargs format.
explain = t.explain().aggregate({$match: {a: 3}});
assert.commandWorked(explain);
assert.eq(1, explain.stages.length);
assert("queryPlanner" in explain.stages[0].$cursor);

explain = t.explain().aggregate({$match: {a: 3}}, {$project: {a: 1}});
assert.commandWorked(explain);
assert.eq(2, explain.stages.length);
assert("queryPlanner" in explain.stages[0].$cursor);

// Options already provided.
explain = t.explain().aggregate([{$match: {a: 3}}], {allowDiskUse: true});
assert.commandWorked(explain);
assert.eq(1, explain.stages.length);
assert("queryPlanner" in explain.stages[0].$cursor);

//
// .count()
//

// Basic count.
explain = t.explain().count();
assert.commandWorked(explain);
assert(planHasStage(explain.queryPlanner.winningPlan, "COUNT"));

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
assert(planHasStage(explain.queryPlanner.winningPlan, "COUNT"));
assert(planHasStage(explain.queryPlanner.winningPlan, "COUNT_SCAN"));

//
// .group()
//

explain = t.explain().group({key: "a", initial: {}, reduce: function() {}});
assert.commandWorked(explain);

//
// .distinct()
//

explain = t.explain().distinct('_id');
assert.commandWorked(explain);
assert(planHasStage(explain.queryPlanner.winningPlan, "PROJECTION"));
assert(planHasStage(explain.queryPlanner.winningPlan, "DISTINCT_SCAN"));

explain = t.explain().distinct('a');
assert.commandWorked(explain);
assert(planHasStage(explain.queryPlanner.winningPlan, "PROJECTION"));
assert(planHasStage(explain.queryPlanner.winningPlan, "DISTINCT_SCAN"));

explain = t.explain().distinct('b');
assert.commandWorked(explain);
assert(planHasStage(explain.queryPlanner.winningPlan, "COLLSCAN"));

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
assert(stage.wouldInsert);

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
assert(stage.wouldInsert);
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
assert(!stage.wouldInsert);
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
assert(!stage.wouldInsert);
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
explain = t.explain("executionStats")
              .findAndModify({query: {a: 15}, update: {$set: {b: 3}}, upsert: true});
assert.commandWorked(explain);
stage = explain.executionStats.executionStages;
if ("SINGLE_SHARD" === stage.stage) {
    stage = stage.shards[0].executionStages;
}
assert.eq(stage.stage, "UPDATE");
assert(stage.wouldInsert);

// Make sure that the insert didn't actually happen.
assert.eq(10, t.count());

//
// Error cases.
//

// Invalid verbosity string.
assert.throws(function() {
    t.explain("foobar").find().finish();
});
assert.throws(function() {
    t.find().explain("foobar");
});

// Can't explain an update without a query.
assert.throws(function() {
    t.explain().update();
});

// Can't explain an update without mods.
assert.throws(function() {
    t.explain().update({a: 3});
});

// Can't add fourth arg when using document-style specification of update options.
assert.throws(function() {
    t.explain().update({a: 3}, {$set: {b: 4}}, {multi: true}, true);
});

// Missing "initial" for explaining a group.
assert.throws(function() {
    t.explain().group({key: "a", reduce: function() {}});
});

// Can't specify both remove and update in a findAndModify
assert.throws(function() {
    t.explain().findAndModify({remove: true, update: {$set: {b: 3}}});
});
