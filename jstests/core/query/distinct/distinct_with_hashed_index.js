/**
 * Test to verify the behaviour of 'distinct' and aggreate with '$group' operations in the presence
 * of compound hashed indexes. In this test we also verify that the query planner uses
 * 'DISTINCT_SCAN' when it is appropriate.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 * ]
 */
import {
    getAggPlanStage,
    getWinningPlanFromExplain,
    isCollscan,
    isIndexOnly,
    isIxscan,
    planHasStage,
} from "jstests/libs/query/analyze_plan.js";

const coll = db.distinct_with_hashed_index;
coll.drop();

for (let i = 0; i < 100; i++) {
    assert.commandWorked(
        assert.commandWorked(coll.insert({a: i, b: {subObj: "str_" + (i % 13)}, c: NumberInt(i % 10)})),
    );
    assert.commandWorked(coll.insert({a: i, b: i % 13, c: NumberInt(i % 10)}));
}

//
// Tests for 'distinct' operation when hashed field is not a prefix.
//
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({a: -1, b: "hashed", c: 1}));

// 'distinct' on non-hashed prefix fields can use DISTINCT_SCAN.
assert.eq(100, coll.distinct("a").length);
let plan = coll.explain("executionStats").distinct("a");
let winningPlan = getWinningPlanFromExplain(plan);
assert(isIndexOnly(db, winningPlan), winningPlan);
assert(planHasStage(db, winningPlan, "DISTINCT_SCAN"), winningPlan);

// 'distinct' on non-prefix fields cannot use index.
assert.eq(26, coll.distinct("b").length);
plan = coll.explain("executionStats").distinct("b");
winningPlan = getWinningPlanFromExplain(plan);
assert(isCollscan(db, winningPlan), winningPlan);
assert.eq(10, coll.distinct("c").length);
plan = coll.explain("executionStats").distinct("c");
winningPlan = getWinningPlanFromExplain(plan);
assert(isCollscan(db, winningPlan), winningPlan);

// A 'distinct' command that cannot use 'DISTINCT_SCAN', can use index scan for the query part.
assert.eq([2], coll.distinct("c", {a: 12, b: {subObj: "str_12"}}));
plan = coll.explain("executionStats").distinct("c", {a: 12, b: {subObj: "str_12"}});
winningPlan = getWinningPlanFromExplain(plan);
assert(isIxscan(db, winningPlan), winningPlan);
assert(!planHasStage(db, winningPlan, "DISTINCT_SCAN"), winningPlan);
assert(planHasStage(db, winningPlan, "FETCH"), winningPlan);

// 'distinct' with query predicate on index field can get converted to DISTINCT_SCAN.
assert.eq([2], coll.distinct("c", {a: 12}));
plan = coll.explain("executionStats").distinct("c", {a: 12});
winningPlan = getWinningPlanFromExplain(plan);
assert(isIndexOnly(db, winningPlan), winningPlan);
assert(planHasStage(db, winningPlan, "DISTINCT_SCAN"), winningPlan);

// Can use index scan to answer the query even when the key of distinct is not eligible from
// DISTINCT_SCAN. Since the query has a point predicate on "b", we need a filter, since the bounds
// on the hashed field are always inexact. We cannot use DISTINCT_SCAN when the plan has FETCH with
// filter.
assert.eq([], coll.distinct("c", {a: 12, b: 4}));
plan = coll.explain("executionStats").distinct("c", {a: 12, b: 4});
winningPlan = getWinningPlanFromExplain(plan);
assert(isIxscan(db, winningPlan), winningPlan);
assert(planHasStage(db, winningPlan, "FETCH"), winningPlan);
assert(!planHasStage(db, winningPlan, "DISTINCT_SCAN"), winningPlan);

// Verify that simple $group on non-hashed field can use DISTINCT_SCAN.
let pipeline = [{$group: {_id: "$a"}}];
assert.eq(100, coll.aggregate(pipeline).itcount());
let explainPlan = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explainPlan, "DISTINCT_SCAN"), explainPlan);

// Verify that simple $group with $match on non-hashed fields can use DISTINCT_SCAN.
pipeline = [{$match: {a: {$lt: 10}}}, {$group: {_id: "$a"}}];
assert.sameMembers(
    [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}, {_id: 7}, {_id: 8}, {_id: 9}],
    coll.aggregate(pipeline).toArray(),
);
explainPlan = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explainPlan, "DISTINCT_SCAN"), explainPlan);

// Verify that simple $group with $match on hashed fields cannot use DISTINCT_SCAN. Since the query
// has a point predicate on "b", we need a filter, since the bounds on the hashed field are always
// inexact. We cannot use DISTINCT_SCAN when the plan has FETCH with filter.
pipeline = [{$match: {a: {$lt: 10}, b: {subObj: "str_8"}}}, {$group: {_id: "$a"}}];
assert.sameMembers([{_id: 8}], coll.aggregate(pipeline).toArray());
explainPlan = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explainPlan, "DISTINCT_SCAN"), explainPlan);
assert.neq(null, getAggPlanStage(explainPlan, "IXSCAN"), explainPlan);
assert.neq(null, getAggPlanStage(explainPlan, "FETCH"), explainPlan);

//
// Tests for 'distinct' operation when hashed field is a prefix.
//
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({b: "hashed", c: 1}));

// 'distinct' on hashed prefix field cannot use index. It is incorrect to use 'DISTINCT_SCAN'
// because of the possibility of hash collision. If the collision happens, we would treat two
// different values as the same and return only the first one.
assert.eq(26, coll.distinct("b").length);
plan = coll.explain("executionStats").distinct("b");
winningPlan = getWinningPlanFromExplain(plan);
assert(isCollscan(db, winningPlan), winningPlan);

// 'distinct' with query predicate can use index for the query part.
assert.eq([1], coll.distinct("b", {b: 1}));
plan = coll.explain("executionStats").distinct("b", {b: 1});
winningPlan = getWinningPlanFromExplain(plan);
assert(isIxscan(db, winningPlan), winningPlan);
assert(planHasStage(db, winningPlan, "FETCH"), winningPlan);

// 'distinct' with query predicate cannot use index when query cannot use index.
assert.eq([5], coll.distinct("b", {b: {$lt: 6, $gt: 4}}));
plan = coll.explain("executionStats").distinct("b", {b: {$lt: 6, $gt: 4}});
winningPlan = getWinningPlanFromExplain(plan);
assert(isCollscan(db, winningPlan), winningPlan);

// 'distinct' with query predicate can use index for the query part.
assert.eq([2], coll.distinct("c", {a: 12, b: 12}));
plan = coll.explain("executionStats").distinct("c", {a: 12, b: 12});
winningPlan = getWinningPlanFromExplain(plan);
assert(isIxscan(db, winningPlan), winningPlan);
assert(planHasStage(db, winningPlan, "FETCH"), winningPlan);

// 'distinct' on non-prefix fields cannot use index.
assert.sameMembers([0, 1, 2, 3, 4, 5, 6, 7, 8, 9], coll.distinct("c"));
plan = coll.explain("executionStats").distinct("c");
winningPlan = getWinningPlanFromExplain(plan);
assert(isCollscan(db, winningPlan), winningPlan);

// Verify that simple $group on hashed field cannot use DISTINCT_SCAN.
pipeline = [{$group: {_id: "$b"}}];
assert.eq(26, coll.aggregate(pipeline).itcount());
explainPlan = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explainPlan, "DISTINCT_SCAN"), explainPlan);
assert.neq(null, getAggPlanStage(explainPlan, "COLLSCAN"), explainPlan);
