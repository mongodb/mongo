// Index bounds generation tests for MinKey values.
// @tags: [requires_non_retryable_writes, assumes_unsharded_collection]
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For assertCoveredQueryAndCount.

const coll = db.index_bounds_minkey;
coll.drop();

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.insert({a: MinKey}));

// Test that queries involving comparison operators with MinKey are covered.
const proj = {
    a: 1,
    _id: 0
};
assertCoveredQueryAndCount({collection: coll, query: {a: {$gt: MinKey}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$gte: MinKey}}, project: proj, count: 1});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lt: MinKey}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lte: MinKey}}, project: proj, count: 1});

// Test that all documents are considered greater than MinKey, regardless of the presence of
// the queried field 'a'.
coll.remove({});
assert.commandWorked(coll.insert({a: "string"}));
assert.commandWorked(coll.insert({a: {b: 1}}));
assert.commandWorked(coll.insert({}));
assertCoveredQueryAndCount({collection: coll, query: {a: {$gt: MinKey}}, project: proj, count: 3});
assertCoveredQueryAndCount({collection: coll, query: {a: {$gte: MinKey}}, project: proj, count: 3});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lt: MinKey}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lte: MinKey}}, project: proj, count: 0});
})();
