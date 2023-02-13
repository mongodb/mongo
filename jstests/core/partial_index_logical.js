/**
 * Test the planners ability to distinguish parameterized queries in the presence of a partial index
 * containing $and.
 *
 * @tags: [
 *  # Since the plan cache is per-node state, this test assumes that all operations are happening
 *  # against the same mongod.
 *  assumes_read_preference_unchanged,
 *  assumes_read_concern_unchanged,
 *  does_not_support_stepdowns,
 *  # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
 *  assumes_balancer_off,
 *  assumes_unsharded_collection,
 *  # Plan cache state is node-local and will not get migrated alongside tenant data.
 *  tenant_migration_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getPlanCacheKeyFromShape.

(function partialIndexMixedFields() {
    db.test.drop();

    // Create enough competing indexes such that a query is eligible for caching (single plan
    // queries are not cached).
    assert.commandWorked(
        db.test.createIndex({num: 1}, {partialFilterExpression: {num: 5, foo: 6}}));
    assert.commandWorked(db.test.createIndex({num: -1}));
    assert.commandWorked(db.test.createIndex({num: -1, not_num: 1}));

    assert.commandWorked(db.test.insert([
        {_id: 0, num: 5, foo: 6},
        {_id: 1, num: 5, foo: 7},
    ]));

    // Run a query which is eligible to use the {num: 1} index as it is covered by the partial
    // filter expression.
    assert.eq(db.test.find({num: 5, foo: 6}).itcount(), 1);
    assert.eq(db.test.find({num: 5, foo: 6}).itcount(), 1);
    const matchingKey =
        getPlanCacheKeyFromShape({query: {num: 5, foo: 6}, collection: db.test, db: db});
    assert.eq(1,
              db.test.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: matchingKey}}])
                  .itcount());

    // This query should not be eligible for the {num: 1} index despite the path 'num' being
    // compatible (per the plan cache key encoding).
    assert.eq(1, db.test.find({num: 5, foo: 7}).itcount());
    const nonCoveredKey =
        getPlanCacheKeyFromShape({query: {num: 5, foo: 7}, collection: db.test, db: db});
    assert.eq(1,
              db.test.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: nonCoveredKey}}])
                  .itcount());

    // Sanity check that the generated keys are different due to the index compatibility.
    assert.neq(nonCoveredKey, matchingKey);
})();

(function partialIndexConjunction() {
    db.test.drop();

    // Create enough competing indexes such that a query is eligible for caching (single plan
    // queries are not cached).
    assert.commandWorked(
        db.test.createIndex({num: 1}, {partialFilterExpression: {num: {$gt: 0, $lt: 10}}}));
    assert.commandWorked(db.test.createIndex({num: -1}));
    assert.commandWorked(db.test.createIndex({num: -1, not_num: 1}));

    assert.commandWorked(db.test.insert([
        {_id: 0},
        {_id: 1, num: 1},
        {_id: 2, num: 11},
    ]));

    // Run a query which is eligible to use the {num: 1} index as it is covered by the partial
    // filter expression.
    assert.eq(db.test.find({num: {$gt: 0, $lt: 10}}).itcount(), 1);
    assert.eq(db.test.find({num: {$gt: 0, $lt: 10}}).itcount(), 1);
    const validKey =
        getPlanCacheKeyFromShape({query: {num: {$gt: 0, $lt: 10}}, collection: db.test, db: db});
    assert.eq(
        1,
        db.test.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: validKey}}]).itcount());

    // The plan for the query above should now be in the cache and active. Now execute a query with
    // a very similar shape, however the predicate parameters are not satisfied by the partial
    // filter expression.
    assert.eq(2, db.test.find({num: {$gt: 0, $lt: 12}}).itcount());
})();
})();
