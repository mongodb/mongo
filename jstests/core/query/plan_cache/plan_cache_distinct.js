/**
 * Test that distinct command and DISTINCT_SCAN optimizations correctly use the plan cache.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: planCacheClear.
 *   not_allowed_with_signed_security_token,
 *   # This test attempts to perform queries and introspect/manipulate the server's plan cache
 *   # entries. The former operation may be routed to a secondary in the replica set, whereas the
 *   # latter must be routed to the primary.
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   does_not_support_stepdowns,
 *   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
 *   assumes_balancer_off,
 *   assumes_unsharded_collection,
 *   # Plan cache state is node-local and will not get migrated alongside tenant data.
 *   tenant_migration_incompatible,
 *   requires_fcv_81,
 *   featureFlagShardFilteringDistinctScan,
 * ]
 */

const coll = db[jsTestName()];

function runAndGetPlanCacheStats(args) {
    if (args.distinct) {
        coll.distinct(args.distinct, args.filter);
    } else {
        coll.aggregate(args.pipeline);
    }
    return coll.aggregate([{$planCacheStats: {}}]).toArray();
}

function assertDistinctUsesPlanCacheCorrectly(distinct, filter) {
    coll.getPlanCache().clear();
    assert.eq(0, coll.aggregate([{$planCacheStats: {}}]).toArray().length);
    let stats = runAndGetPlanCacheStats({distinct: distinct, filter: filter});
    assert.eq(1, stats.length);
    assert.eq(false, stats[0].isActive);
    assert.eq(filter, stats[0].createdFromQuery.query, stats);
    assert.eq({}, stats[0].createdFromQuery.sort, stats);
    assert.eq({}, stats[0].createdFromQuery.projection, stats);

    stats = runAndGetPlanCacheStats({distinct: distinct, filter: filter});
    assert.eq(1, stats.length);
    assert.eq(true, stats[0].isActive);
}

function assertAggDistinctUsesPlanCacheCorrectly(pipeline) {
    coll.getPlanCache().clear();
    assert.eq(0, coll.aggregate([{$planCacheStats: {}}]).toArray().length);
    let stats = runAndGetPlanCacheStats({pipeline: pipeline});
    assert.eq(1, stats.length);
    assert.eq(false, stats[0].isActive);

    stats = runAndGetPlanCacheStats({pipeline: pipeline});
    assert.eq(1, stats.length);
    assert.eq(true, stats[0].isActive);
}

{
    coll.drop();
    coll.createIndex({x: 1, y: 1});
    coll.createIndex({y: 1, x: 1});
    coll.createIndex({x: 1, z: 1, y: 1});
    coll.insertMany([
        {x: 3, y: 5, z: 7},
        {x: 5, y: 6, z: 5},
        {x: 5, y: 5, z: 4},
        {x: 6, y: 5, z: 3},
        {x: 6, y: 5, z: 7},
        {x: 7, y: 5, z: 8},
        {x: 7, y: 5, z: 9},
        {x: 8, y: 7, z: 3},
        {x: 8, y: 8, z: 3},
        {x: 8, y: 5, z: 5},
        {x: 9, y: 5, z: 3},
        {x: 9, y: 5, z: 4},
    ]);

    assertDistinctUsesPlanCacheCorrectly("x", {x: {$gt: 3}, y: 5});

    // Confirm that a different predicate with a matching shape uses plan cache.
    let stats = runAndGetPlanCacheStats({distinct: "x", filter: {x: {$gt: 5}, y: 5}});
    assert.eq(true, stats[0].isActive);
}

{
    coll.drop();
    for (let i = 0; i < 100; ++i) {
        coll.insert({x: i % 2, y: i + 100, z: i + 200});
    }
    coll.createIndex({x: 1});
    coll.createIndex({x: 1, y: 1});
    coll.createIndex({y: 1, z: 1});
    coll.createIndex({x: 1, y: 1, z: 1});

    assertDistinctUsesPlanCacheCorrectly("x", {x: {$gt: 12}, y: {$lt: 200}});
}

{
    // Prefer cached IXSCAN for no duplicate values in the collection.
    coll.drop();
    for (let i = 0; i < 100; ++i)
        coll.insert({x: i, y: i + 100, z: i + 200});
    coll.createIndex({x: 1});
    coll.createIndex({x: 1, y: 1});
    coll.createIndex({y: 1, z: 1});

    assertDistinctUsesPlanCacheCorrectly("x", {x: {$gt: -1}, y: {$lt: 105}});
}

{
    coll.drop();
    coll.createIndex({a: 1});
    coll.createIndex({b: 1});
    coll.createIndex({a: 1, b: 1});
    coll.createIndex({a: -1, b: 1});
    coll.createIndex({a: 1, b: -1});
    coll.createIndex({a: 1, b: 1, c: 1});
    coll.createIndex({b: 1, a: 1});
    coll.createIndex({b: 1, c: 1});

    coll.insertMany([
        {_id: 1, a: 4, b: 2, c: 3, d: 4},
        {_id: 2, a: 4, b: 3, c: 6, d: 5},
        {_id: 3, a: 5, b: 4, c: 7, d: 5}
    ]);

    // Choose DISTINCT_SCAN from plan cache.
    assertAggDistinctUsesPlanCacheCorrectly(
        [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$first: "$b"}}}]);

    // $group with $top/$bottom uses plan cache
    assertAggDistinctUsesPlanCacheCorrectly(
        [{$group: {_id: "$a", accum: {$top: {sortBy: {a: 1, b: 1}, output: "$c"}}}}]);
    assertAggDistinctUsesPlanCacheCorrectly(
        [{$group: {_id: "$a", accum: {$bottom: {sortBy: {a: -1, b: -1}, output: "$c"}}}}]);

    assertAggDistinctUsesPlanCacheCorrectly([
        {$match: {a: {$ne: 4}}},
        {$group: {_id: "$a", accum: {$top: {sortBy: {a: 1, b: 1}, output: "$b"}}}}
    ]);
    // Confirm that a different pipeline with a matching shape uses plan cache.
    let stats = runAndGetPlanCacheStats({
        pipeline: [
            {$match: {a: {$ne: 5}}},
            {$group: {_id: "$a", accum: {$top: {sortBy: {a: 1, b: 1}, output: "$b"}}}}
        ]
    });
    assert.eq(true, stats[0].isActive);

    coll.insertMany([{a: 4, b: 2, c: 3}, {a: 4, b: 3, c: 6}, {a: 5, b: 4, c: 7, d: [1, 2, 3]}]);

    // Choose DISTINCT_SCAN from plan cache over IXSCAN.
    assertAggDistinctUsesPlanCacheCorrectly(
        [{$sort: {a: -1, b: -1}}, {$group: {_id: "$a", accum: {$last: "$b"}}}]);
}
