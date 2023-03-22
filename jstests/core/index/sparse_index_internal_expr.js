/*
 * Tests that a sparse index cannot be used to answer a $expr query unless the sparse index is
 * explicitly hinted. If a sparse index is hinted to answer a $expr query, incomplete results could
 * be returned.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const coll = db.sparse_index_internal_expr;
coll.drop();

coll.insert({a: 1});

const exprQuery = {
    $expr: {$lt: ["$missing", "r"]}
};

// Run a query with $expr on a missing field. This query will use a COLLSCAN plan and return
// document '{a: 1}' because $expr expression does not apply type bracketing, specifically, the
// missing field is evaluated to 'null'. The expression returns "true" because 'null' < "r".
let res = coll.find(exprQuery, {_id: 0}).toArray();

assert.eq(res.length, 1);
assert.docEq(res[0], {a: 1});

// Tests that a non-sparse index {missing: 1} can be used to answer the $expr query.
assert.commandWorked(coll.createIndex({"missing": 1}));

// Explain the query, and determine whether an indexed solution is available.
let ixScans = getPlanStages(getWinningPlan(coll.find(exprQuery).explain().queryPlanner), "IXSCAN");

// Verify that the winning plan uses the $** index with the expected bounds.
assert.gt(ixScans.length, 0, ixScans);
assert.eq("missing_1", ixScans[0].indexName, ixScans);

// Run the same query. A complete result will be returned.
res = coll.find(exprQuery, {_id: 0}).toArray();
assert.eq(res.length, 1);
assert.docEq(res[0], {a: 1});

// Drop the non-sparse index and create a sparse index with the same key pattern.
assert.commandWorked(coll.dropIndex("missing_1"));
assert.commandWorked(coll.createIndex({'missing': 1}, {'sparse': true}));

// Run the same query to test that a COLLSCAN plan is used rather than an indexed plan.
const collScans =
    getPlanStages(getWinningPlan(coll.find(exprQuery).explain().queryPlanner), "COLLSCAN");

// Verify that the winning plan uses the $** index with the expected bounds.
assert.gt(collScans.length, 0, collScans);

// Test that a sparse index can be hinted to answer $expr query but incomplete results in returned,
// because the document is not indexed by the sparse index.
res = coll.find(exprQuery, {_id: 0}).hint("missing_1").toArray();
assert.eq(res.length, 0);

ixScans = getPlanStages(
    getWinningPlan(coll.find(exprQuery).hint("missing_1").explain().queryPlanner), "IXSCAN");

assert.gt(ixScans.length, 0, ixScans);
assert.eq("missing_1", ixScans[0].indexName, ixScans);
assert.eq(true, ixScans[0].isSparse, ixScans);
}());
