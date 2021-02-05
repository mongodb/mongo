/**
 * Tests that we can execute a query which survived a yield using an index scan on a path containing
 * a positional component. This test was designed to reproduce SERVER-52589.
 *
 * @tags: [
 *   requires_find_command,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");         // For explain helpers.
load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.

// Configure 'internalQueryExecYieldIterations' such that operations will yield on each PlanExecutor
// iteration.
const options = {
    setParameter: {internalQueryExecYieldIterations: 1}
};
const conn = MongoRunner.runMongod(options);
assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(options));

const testDb = conn.getDB("test");
const coll = testDb.multikey_opt;

// Create a compound index where one index key contains a positional path component.
assert.commandWorked(coll.createIndex({"a.0.b": 1, c: 1}));

// Insert one document where field 'a' is an array with one element. Despite the fact that 'a' is an
// array, since we index on a particular element of the array, the index should not be turned into
// multikey by this operation.
const doc = {
    a: [{b: 1}],
    c: 1
};
assert.commandWorked(coll.insert(doc));

// Query the collection by both index fields, and sort by field 'c'. This query will result in a
// FETCH - SORT - IXSCAN plan. This shape of the plan is important for this test, as described in
// SERVER-52589.
const cursor = coll.find({"a.0.b": {$gt: 0}, c: {$gt: 0}}).sort({c: 1});

// Explain and ensure that the plan is FETCH - SORT - IXSCAN on a non-multikey index.
const explain = cursor.explain();

const fetch = getPlanStage(explain, "FETCH");
assert.neq(fetch, null, explain);
assert(fetch.hasOwnProperty("inputStage"), explain);

const sort = fetch.inputStage;
assert.neq(sort, null, explain);
assert(sort.hasOwnProperty("stage"), explain);
assert.eq(sort.stage, "SORT", explain);
assert(sort.hasOwnProperty("inputStage"), explain);

const ixscan = sort.inputStage;
assert.neq(ixscan, null, explain);
assert(ixscan.hasOwnProperty("stage"), explain);
assert.eq(ixscan.stage, "IXSCAN", explain);
assert.eq(ixscan.isMultiKey, false, explain);

// Now execute the query and validate the result.
assertArrayEq(cursor.toArray(), [doc]);

MongoRunner.stopMongod(conn);
}());
