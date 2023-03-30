/**
 * Test that a query eligible for the "explode for sort" optimization works correctly when the query
 * involves an equality-to-array predicate. Specifically, we use an `$all` where the constants
 * inside the `$all` list are singleton arrays rather than scalars.
 *
 * This test was originally designed to reproduce SERVER-75304.
 *
 * @tags: [
 *   # explain does not support majority read concern
 *   assumes_read_concern_local,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());
const coll = testDB.explode_for_sort_equality_to_array;

assert.commandWorked(coll.createIndex({array: -1, num: 1}));
assert.commandWorked(coll.insert({array: [[1], [2]]}));
assert.commandWorked(coll.insert({array: [[1]]}));
assert.commandWorked(coll.insert({array: [[2]]}));
const explain = assert.commandWorked(
    coll.find({array: {$all: [[1], [2]]}}).sort({num: 1}).explain('executionStats'));
assert.gt(
    getPlanStages(getWinningPlan(explain.queryPlanner), "SORT_MERGE").length, 0, tojson(explain));
assert.eq(1, explain.executionStats.nReturned, tojson(explain));
}());
