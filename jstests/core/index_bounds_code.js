// Index bounds generation tests for Code/CodeWSCope values.
// @tags: [requires_non_retryable_writes, assumes_unsharded_collection]
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For assertCoveredQueryAndCount.

const coll = db.index_bounds_code;
coll.drop();

assert.commandWorked(coll.createIndex({a: 1}));
const insertedFunc = function() {
    return 1;
};
assert.commandWorked(coll.insert({a: insertedFunc}));

// Test that queries involving comparison operators with values of type Code are covered.
const proj = {
    a: 1,
    _id: 0
};
const func = function() {
    return 2;
};
assertCoveredQueryAndCount({collection: coll, query: {a: {$gt: func}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$gte: func}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lt: func}}, project: proj, count: 1});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lte: func}}, project: proj, count: 1});

// Test for equality against the original inserted function.
assertCoveredQueryAndCount(
    {collection: coll, query: {a: {$gt: insertedFunc}}, project: proj, count: 0});
assertCoveredQueryAndCount(
    {collection: coll, query: {a: {$gte: insertedFunc}}, project: proj, count: 1});
assertCoveredQueryAndCount(
    {collection: coll, query: {a: {$lt: insertedFunc}}, project: proj, count: 0});
assertCoveredQueryAndCount(
    {collection: coll, query: {a: {$lte: insertedFunc}}, project: proj, count: 1});

// Test that documents that lie outside of the generated index bounds are not returned.
coll.remove({});
assert.commandWorked(coll.insert({a: "string"}));
assert.commandWorked(coll.insert({a: {b: 1}}));
assert.commandWorked(coll.insert({a: MaxKey}));

assertCoveredQueryAndCount({collection: coll, query: {a: {$gt: func}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$gte: func}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lt: func}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lte: func}}, project: proj, count: 0});
})();
