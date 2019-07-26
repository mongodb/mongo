// Index bounds generation tests for MaxKey values.
// @tags: [requires_non_retryable_writes, assumes_unsharded_collection]
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For assertCoveredQueryAndCount.

const coll = db.index_bounds_maxkey;
coll.drop();

assert.commandWorked(coll.createIndex({a: 1}));
assert.writeOK(coll.insert({a: MaxKey}));

// Test that queries involving comparison operators with MaxKey are covered.
const proj = {
    a: 1,
    _id: 0
};
assertCoveredQueryAndCount({collection: coll, query: {a: {$gt: MaxKey}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$gte: MaxKey}}, project: proj, count: 1});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lt: MaxKey}}, project: proj, count: 1});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lte: MaxKey}}, project: proj, count: 1});

// Test that all documents are considered less than MaxKey, regardless of the presence of
// the queried field 'a'.
coll.remove({});
assert.writeOK(coll.insert({a: "string"}));
assert.writeOK(coll.insert({a: {b: 1}}));
assert.writeOK(coll.insert({}));
assertCoveredQueryAndCount({collection: coll, query: {a: {$gt: MaxKey}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$gte: MaxKey}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lt: MaxKey}}, project: proj, count: 3});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lte: MaxKey}}, project: proj, count: 3});
})();
