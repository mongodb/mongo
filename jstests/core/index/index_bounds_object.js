// Index bounds generation tests for Object values.
// @tags: [
//   assumes_unsharded_collection,
//   requires_non_retryable_writes,
//   requires_fcv_82,
// ]
import {assertCoveredQueryAndCount, getWinningPlanFromExplain, isIndexOnly} from "jstests/libs/query/analyze_plan.js";

const coll = db.index_bounds_object;
coll.drop();

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.insert({a: {b: 1}}));

// Test that queries involving comparison operators with objects are covered.
const proj = {
    a: 1,
    _id: 0,
};
assertCoveredQueryAndCount({collection: coll, query: {a: {$gt: {b: 0}}}, project: proj, count: 1});
assertCoveredQueryAndCount({collection: coll, query: {a: {$gt: {b: 2}}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$gte: {b: 1}}}, project: proj, count: 1});
assertCoveredQueryAndCount({collection: coll, query: {a: {$gte: {b: 1, c: 2}}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lt: {b: 2}}}, project: proj, count: 1});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lte: {b: 1}}}, project: proj, count: 1});

// Test that queries involving comparisons with an empty object are covered.
assert.commandWorked(coll.insert({a: {}}));
assertCoveredQueryAndCount({collection: coll, query: {a: {$gt: {}}}, project: proj, count: 1});
assertCoveredQueryAndCount({collection: coll, query: {a: {$gte: {}}}, project: proj, count: 2});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lt: {}}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lte: {}}}, project: proj, count: 1});

// Test that queries involving comparisons with a range of objects are covered.
assertCoveredQueryAndCount({collection: coll, query: {a: {$gt: {}, $lt: {b: 2}}}, project: proj, count: 1});
assertCoveredQueryAndCount({collection: coll, query: {a: {$gte: {}, $lt: {b: 2}}}, project: proj, count: 2});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lt: {}, $gte: {}}}, project: proj, count: 0});

// Test that documents that lie outside of the generated index bounds are not returned. Cannot
// test empty array upper bounds since that would force the index to be multi-key.
coll.remove({});
assert.commandWorked(coll.insert({a: "string"}));
assert.commandWorked(coll.insert({a: true}));
assertCoveredQueryAndCount({collection: coll, query: {a: {$gt: {}}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$gte: {}}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lt: {}}}, project: proj, count: 0});
assertCoveredQueryAndCount({collection: coll, query: {a: {$lte: {}}}, project: proj, count: 0});

// Adding a document containing an array makes the index multi-key which can never be used for a
// covered query.
assert.commandWorked(coll.insert({a: []}));
assert(!isIndexOnly(db, getWinningPlanFromExplain(coll.find({a: {$gt: {}}}, proj).explain())));
assert(!isIndexOnly(db, getWinningPlanFromExplain(coll.find({a: {$gte: {}}}, proj).explain())));
assert(!isIndexOnly(db, getWinningPlanFromExplain(coll.find({a: {$lt: {}}}, proj).explain())));
assert(!isIndexOnly(db, getWinningPlanFromExplain(coll.find({a: {$lte: {}}}, proj).explain())));
