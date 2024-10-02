/**
 * Test the planners ability to distinguish parameterized queries in the presence of a partial index
 * containing logical expressions ($and, $or).
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
 *  requires_fcv_63,
 *  # Plan cache state is node-local and will not get migrated alongside tenant data.
 *  tenant_migration_incompatible,
 * ]
 */
import {getPlanCacheKeyFromShape} from "jstests/libs/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();
(function partialIndexMixedFields() {
    coll.drop();

    // Create enough competing indexes such that a query is eligible for caching (single plan
    // queries are not cached).
    assert.commandWorked(coll.createIndex({num: 1}, {partialFilterExpression: {num: 5, foo: 6}}));
    assert.commandWorked(coll.createIndex({num: -1}));
    assert.commandWorked(coll.createIndex({num: 1, not_num: 1}));

    assert.commandWorked(coll.insert([
        {_id: 0, num: 5, foo: 6},
        {_id: 1, num: 5, foo: 7},
    ]));

    // Run a query which is eligible to use the {num: 1} index as it is covered by the partial
    // filter expression.
    assert.eq(coll.find({num: 5, foo: 6}).itcount(), 1);
    assert.eq(coll.find({num: 5, foo: 6}).itcount(), 1);
    const matchingKey =
        getPlanCacheKeyFromShape({query: {num: 5, foo: 6}, collection: coll, db: db});
    assert.eq(
        1,
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: matchingKey}}]).itcount());

    // This query should not be eligible for the {num: 1} index despite the path 'num' being
    // compatible (per the plan cache key encoding).
    assert.eq(1, coll.find({num: 5, foo: 7}).itcount());
    const nonCoveredKey =
        getPlanCacheKeyFromShape({query: {num: 5, foo: 7}, collection: coll, db: db});
    assert.eq(
        1,
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: nonCoveredKey}}]).itcount());

    // Sanity check that the generated keys are different due to the index compatibility.
    assert.neq(nonCoveredKey, matchingKey);
})();

(function partialIndexDisjunction() {
    coll.drop();

    // Create enough competing indexes such that a query is eligible for caching (single plan
    // queries are not cached).
    assert.commandWorked(coll.createIndex(
        {num: 1},
        {partialFilterExpression: {$or: [{num: {$exists: true}}, {num: {$type: 'number'}}]}}));
    assert.commandWorked(coll.createIndex({num: -1}));
    assert.commandWorked(coll.createIndex({num: 1, not_num: 1}));

    assert.commandWorked(coll.insert([
        {_id: 0},
        {_id: 1, num: null},
        {_id: 2, num: 5},
    ]));

    // Run a query which is eligible to use the {num: 1} index as it is covered by the partial
    // filter expression.
    assert.eq(coll.find({num: 5}).itcount(), 1);
    assert.eq(coll.find({num: 5}).itcount(), 1);
    const numericKey = getPlanCacheKeyFromShape({query: {num: 5}, collection: coll, db: db});
    assert.eq(
        1, coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: numericKey}}]).itcount());

    // The plan for the query above should now be in the cache and active. Now execute a query with
    // a very similar shape, however the predicate parameters are not satisfied by the partial
    // filter expression. This is because {num: null} should match both explicit null as well as
    // missing values (the latter are not indexed).
    assert.eq(2, coll.find({num: null}).itcount());
    const nullKey = getPlanCacheKeyFromShape({query: {num: null}, collection: coll, db: db});
    assert.eq(1,
              coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: nullKey}}]).itcount());

    // Sanity check that the generated keys are different due to the index compatibility.
    assert.neq(nullKey, numericKey);
})();

(function partialIndexDisjunctionWithCollation() {
    coll.drop();

    const caseInsensitive = {locale: "en_US", strength: 2};

    // Create enough competing indexes such that a query is eligible for caching (single plan
    // queries are not cached).
    assert.commandWorked(coll.createIndex({a: 1}, {
        partialFilterExpression: {$or: [{a: {$gt: 0}}, {a: {$gt: ""}}]},
        collation: caseInsensitive,
    }));
    assert.commandWorked(coll.createIndex({a: -1}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));

    assert.commandWorked(coll.insert([
        {_id: 0, a: "some"},
        {_id: 1, a: "string"},
    ]));

    // Populate the plan cache for a query which is eligible for the partial index. This is true
    // without an explicit collation because the query text does not contain any string comparisons.
    assert.eq(coll.aggregate({$match: {a: {$in: [1, 3]}}}).itcount(), 0);
    assert.eq(coll.aggregate({$match: {a: {$in: [1, 3]}}}).itcount(), 0);
    const simpleCollationKey =
        getPlanCacheKeyFromShape({query: {a: {$in: [1, 3]}}, collection: coll, db: db});
    assert.eq(1,
              coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: simpleCollationKey}}])
                  .itcount());

    // A collation-sensitive query should _not_ use the cached plan since the default simple
    // collation does not match the collation on the index.
    assert.eq(coll.aggregate({$match: {a: {$in: ["a", "Some"]}}}).itcount(), 0);
    assert.eq(coll.aggregate({$match: {a: {$in: ["a", "Some"]}}}).itcount(), 0);
    const collationSensitiveKey =
        getPlanCacheKeyFromShape({query: {a: {$in: ["a", "Some"]}}, collection: coll, db: db});
    assert.eq(
        1,
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: collationSensitiveKey}}])
            .itcount());

    // Sanity check that the generated keys are different due to the collation and index
    // compatibility.
    assert.neq(collationSensitiveKey, simpleCollationKey);
})();

(function partialIndexConjunction() {
    coll.drop();

    // Create enough competing indexes such that a query is eligible for caching (single plan
    // queries are not cached).
    assert.commandWorked(
        coll.createIndex({num: 1}, {partialFilterExpression: {num: {$gt: 0, $lt: 10}}}));
    assert.commandWorked(coll.createIndex({num: -1}));
    assert.commandWorked(coll.createIndex({num: 1, not_num: 1}));

    assert.commandWorked(coll.insert([
        {_id: 0},
        {_id: 1, num: 1},
        {_id: 2, num: 11},
    ]));

    // Run a query which is eligible to use the {num: 1} index as it is covered by the partial
    // filter expression.
    assert.eq(coll.find({num: {$gt: 0, $lt: 10}}).itcount(), 1);
    assert.eq(coll.find({num: {$gt: 0, $lt: 10}}).itcount(), 1);
    const validKey =
        getPlanCacheKeyFromShape({query: {num: {$gt: 0, $lt: 10}}, collection: coll, db: db});
    assert.eq(
        1, coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: validKey}}]).itcount());

    // The plan for the query above should now be in the cache and active. Now execute a query with
    // a very similar shape, however the predicate parameters are not satisfied by the partial
    // filter expression.
    assert.eq(2, coll.find({num: {$gt: 0, $lt: 12}}).itcount());
})();