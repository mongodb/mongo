/**
 * Tests for distinct planning and execution in the presence of multikey indexes.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

let coll = db.jstest_distinct_multikey;
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.insert({a: [1, 2, 3]}));
assert.commandWorked(coll.insert({a: [2, 3, 4]}));
assert.commandWorked(coll.insert({a: [5, 6, 7]}));

// Test that distinct can correctly use a multikey index when there is no predicate.
let result = coll.distinct("a");
assert.eq([1, 2, 3, 4, 5, 6, 7], result.sort());
let explain = coll.explain("queryPlanner").distinct("a");
let winningPlan = getWinningPlan(explain.queryPlanner);
assert(planHasStage(db, winningPlan, "PROJECTION_COVERED"));
assert(planHasStage(db, winningPlan, "DISTINCT_SCAN"));

// Test that distinct can correctly use a multikey index when there is a predicate. This query
// should not be eligible for the distinct scan and cannot be covered.
result = coll.distinct("a", {a: 3});
assert.eq([1, 2, 3, 4], result.sort());
explain = coll.explain("queryPlanner").distinct("a", {a: 3});
winningPlan = getWinningPlan(explain.queryPlanner);
assert(planHasStage(db, winningPlan, "FETCH"));
assert(planHasStage(db, winningPlan, "IXSCAN"));

// Test distinct over a dotted multikey field, with a predicate.
assert(coll.drop());
assert.commandWorked(coll.createIndex({"a.b": 1}));
assert.commandWorked(coll.insert({a: {b: [1, 2, 3]}}));
assert.commandWorked(coll.insert({a: {b: [2, 3, 4]}}));

result = coll.distinct("a.b", {"a.b": 3});
assert.eq([1, 2, 3, 4], result.sort());
explain = coll.explain("queryPlanner").distinct("a.b", {"a.b": 3});
winningPlan = getWinningPlan(explain.queryPlanner);
assert(planHasStage(db, winningPlan, "FETCH"));
assert(planHasStage(db, winningPlan, "IXSCAN"));

// Test that the distinct scan can be used when there is a predicate and the index is not
// multikey.
assert(coll.drop());
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 2}));
assert.commandWorked(coll.insert({a: 3}));

result = coll.distinct("a", {a: {$gte: 2}});
assert.eq([2, 3], result.sort());
explain = coll.explain("queryPlanner").distinct("a", {a: {$gte: 2}});
winningPlan = getWinningPlan(explain.queryPlanner);
assert(planHasStage(db, winningPlan, "PROJECTION_COVERED"));
assert(planHasStage(db, winningPlan, "DISTINCT_SCAN"));

// Test a distinct which can use a multikey index, where the field being distinct'ed is not
// multikey.
assert(coll.drop());
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: 1, b: [2, 3]}));
assert.commandWorked(coll.insert({a: 8, b: [3, 4]}));
assert.commandWorked(coll.insert({a: 7, b: [4, 5]}));

result = coll.distinct("a", {a: {$gte: 2}});
assert.eq([7, 8], result.sort());
explain = coll.explain("queryPlanner").distinct("a", {a: {$gte: 2}});
winningPlan = getWinningPlan(explain.queryPlanner);
assert(planHasStage(db, winningPlan, "PROJECTION_COVERED"));
assert(planHasStage(db, winningPlan, "DISTINCT_SCAN"));

// Test distinct over a trailing multikey field.
result = coll.distinct("b", {a: {$gte: 2}});
assert.eq([3, 4, 5], result.sort());
explain = coll.explain("queryPlanner").distinct("b", {a: {$gte: 2}});
winningPlan = getWinningPlan(explain.queryPlanner);
assert(planHasStage(db, winningPlan, "FETCH"));
assert(planHasStage(db, winningPlan, "IXSCAN"));

// Test distinct over a trailing non-multikey field, where the leading field is multikey.
assert(coll.drop());
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: [2, 3], b: 1}));
assert.commandWorked(coll.insert({a: [3, 4], b: 8}));
assert.commandWorked(coll.insert({a: [3, 5], b: 7}));

result = coll.distinct("b", {a: 3});
assert.eq([1, 7, 8], result.sort());
explain = coll.explain("queryPlanner").distinct("b", {a: 3});
winningPlan = getWinningPlan(explain.queryPlanner);
assert(planHasStage(db, winningPlan, "PROJECTION_COVERED"));
assert(planHasStage(db, winningPlan, "DISTINCT_SCAN"));

// Test distinct over a trailing non-multikey dotted path where the leading field is multikey.
assert(coll.drop());
assert.commandWorked(coll.createIndex({a: 1, "b.c": 1}));
assert.commandWorked(coll.insert({a: [2, 3], b: {c: 1}}));
assert.commandWorked(coll.insert({a: [3, 4], b: {c: 8}}));
assert.commandWorked(coll.insert({a: [3, 5], b: {c: 7}}));

result = coll.distinct("b.c", {a: 3});
assert.eq([1, 7, 8], result.sort());
explain = coll.explain("queryPlanner").distinct("b.c", {a: 3});
winningPlan = getWinningPlan(explain.queryPlanner);
assert(planHasStage(db, winningPlan, "PROJECTION_DEFAULT"));
assert(planHasStage(db, winningPlan, "DISTINCT_SCAN"));
}());
