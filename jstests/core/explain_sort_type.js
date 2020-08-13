/**
 * Test that explain reports the sort algorithm used for a blocking sort -- either "simple" or
 * "default".
 *
 * @tags: [
 *   # Shard filtering may be required if the collection is sharded, which could affect the query
 *   # planner's selection of the "simple" versus "default" sort algorithm.
 *   assumes_unsharded_collection,
 *   # This test uses a non-retryable multi-update command.
 *   requires_non_retryable_writes,
 *   sbe_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const coll = db.explain_sort_type;
coll.drop();

let explain, sortStage;

assert.commandWorked(coll.insert({_id: 0, a: 1, b: 1}));
assert.commandWorked(coll.insert({_id: 1, a: 1, b: 2}));
assert.commandWorked(coll.insert({_id: 2, a: 1, b: 3}));
assert.commandWorked(coll.insert({_id: 3, a: 2, b: 1}));
assert.commandWorked(coll.insert({_id: 4, a: 2, b: 2}));
assert.commandWorked(coll.insert({_id: 5, a: 2, b: 3}));

// Without any indexes, the plan is to scan the collection and run a blocking SORT. This can use the
// simple sort algorithm.
assert.eq(
    [
        {_id: 0, a: 1, b: 1},
        {_id: 3, a: 2, b: 1},
        {_id: 1, a: 1, b: 2},
        {_id: 4, a: 2, b: 2},
        {_id: 2, a: 1, b: 3},
        {_id: 5, a: 2, b: 3}
    ],
    coll.find().sort({b: 1, a: 1}).toArray());
explain = coll.find().sort({b: 1, a: 1}).explain();
sortStage = getPlanStage(explain.queryPlanner.winningPlan, "SORT");
assert.neq(null, sortStage, explain);
assert.eq("simple", sortStage.type, explain);

// If sort key metadata is requested, then we still allow the "simple" algorithm.
assert.eq(
    [
        {_id: 0, a: 1, b: 1, key: [1, 1]},
        {_id: 3, a: 2, b: 1, key: [1, 2]},
        {_id: 1, a: 1, b: 2, key: [2, 1]},
        {_id: 4, a: 2, b: 2, key: [2, 2]},
        {_id: 2, a: 1, b: 3, key: [3, 1]},
        {_id: 5, a: 2, b: 3, key: [3, 2]}
    ],
    coll.find({}, {key: {$meta: "sortKey"}}).sort({b: 1, a: 1}).toArray());
explain = coll.find({}, {key: {$meta: "sortKey"}}).sort({b: 1, a: 1}).explain();
sortStage = getPlanStage(explain.queryPlanner.winningPlan, "SORT");
assert.neq(null, sortStage, explain);
assert.eq("simple", sortStage.type, explain);

// When the blokcing sort is covered, operating on index key data, we use the "default" algorithm.
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
assert.eq([{a: 1, b: 1}, {a: 2, b: 1}, {a: 1, b: 2}, {a: 2, b: 2}, {a: 1, b: 3}, {a: 2, b: 3}],
          coll.find({a: {$gt: 0}}, {_id: 0, a: 1, b: 1}).sort({b: 1, a: 1}).toArray());
explain = coll.find({a: {$gt: 0}}, {_id: 0, a: 1, b: 1}).sort({b: 1, a: 1}).explain();
// Verify that the plan involves an IXSCAN but no fetch.
assert.neq(null, getPlanStage(explain.queryPlanner.winningPlan, "IXSCAN"), explain);
assert.eq(null, getPlanStage(explain.queryPlanner.winningPlan, "FETCH"), explain);
sortStage = getPlanStage(explain.queryPlanner.winningPlan, "SORT");
assert.neq(null, sortStage, explain);
assert.eq("default", sortStage.type, explain);

// If metadata other than "sortKey" is needed, then we fall back to the "default" algorithm. Here we
// show that when "textScore" metadata is attached to the documents in the result set, the sort uses
// the "default" algorithm.
assert.commandWorked(coll.createIndex({c: "text"}));
assert.commandWorked(coll.update({}, {$set: {c: "keyword"}}, {multi: true}));
assert.eq(
    [
        {a: 1, b: 1, score: 1.1},
        {a: 2, b: 1, score: 1.1},
        {a: 1, b: 2, score: 1.1},
        {a: 2, b: 2, score: 1.1},
        {a: 1, b: 3, score: 1.1},
        {a: 2, b: 3, score: 1.1}
    ],
    coll.find({$text: {$search: "keyword"}}, {_id: 0, a: 1, b: 1, score: {$meta: "textScore"}})
        .sort({b: 1, a: 1})
        .toArray());
explain =
    coll.find({$text: {$search: "keyword"}}, {_id: 0, a: 1, b: 1, score: {$meta: "textScore"}})
        .sort({b: 1, a: 1})
        .explain();
sortStage = getPlanStage(explain.queryPlanner.winningPlan, "SORT");
assert.neq(null, sortStage, explain);
assert.eq("default", sortStage.type, explain);
}());
