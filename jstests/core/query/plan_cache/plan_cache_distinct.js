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
 *   requires_fcv_81,
 *   featureFlagShardFilteringDistinctScan,
 *   requires_getmore,
 * ]
 */

import {
    getPlanCacheKeyFromExplain,
    getPlanCacheShapeHashFromExplain
} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];

// Runs the distinct query or aggregation pipelines and returns the query results and the state of
// the plan cache for the relevant query shape.
function runAndGetResultsAndPlanCacheStats(args) {
    let results;
    let explain;
    if (args.distinct) {
        results = coll.distinct(args.distinct, args.filter);
        explain = coll.explain().distinct(args.distinct, args.filter);
    } else {
        results = coll.aggregate(args.pipeline).toArray();
        explain = coll.explain().aggregate(args.pipeline);
    }
    const planCacheShapeHash = getPlanCacheShapeHashFromExplain(explain);
    return [
        results,
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheShapeHash}}]).toArray()
    ];
}

function assertDistinctUsesPlanCacheCorrectly(distinct, filter) {
    coll.getPlanCache().clear();
    assert.eq(0, coll.aggregate([{$planCacheStats: {}}]).toArray().length);
    let [resultsA, statsA] =
        runAndGetResultsAndPlanCacheStats({distinct: distinct, filter: filter});
    assert.eq(1, statsA.length);
    assert.eq(false, statsA[0].isActive);
    assert.eq(filter, statsA[0].createdFromQuery.query, statsA);
    assert.eq({}, statsA[0].createdFromQuery.sort, statsA);
    assert.eq({}, statsA[0].createdFromQuery.projection, statsA);

    let [resultsB, statsB] =
        runAndGetResultsAndPlanCacheStats({distinct: distinct, filter: filter});
    assert.eq(1, statsB.length);
    assert.eq(true, statsB[0].isActive);

    assert.eq(resultsA.length, resultsB.length);
    resultsA.every(doc => resultsB.includes(doc));
}

function assertAggDistinctUsesPlanCacheCorrectly(pipeline) {
    coll.getPlanCache().clear();
    assert.eq(0, coll.aggregate([{$planCacheStats: {}}]).toArray().length);
    let [resultsA, statsA] = runAndGetResultsAndPlanCacheStats({pipeline: pipeline});
    assert.eq(1, statsA.length);
    assert.eq(false, statsA[0].isActive);

    let [resultsB, statsB] = runAndGetResultsAndPlanCacheStats({pipeline: pipeline});
    assert.eq(1, statsB.length);
    assert.eq(true, statsB[0].isActive);

    assert.eq(resultsA.length, resultsB.length);
    resultsA.every(doc => resultsB.includes(doc));
}

function confirmDifferentPlanCacheEntries(pipelineA, pipelineB) {
    coll.getPlanCache().clear();
    let [resultsA, statsA] = runAndGetResultsAndPlanCacheStats({pipeline: pipelineA});
    assert.eq(1, statsA.length);
    assert.eq(false, statsA[0].isActive);

    let [resultsB, statsB] = runAndGetResultsAndPlanCacheStats({pipeline: pipelineB});
    assert.eq(1, statsB.length);
    assert.eq(false, statsB[0].isActive);
    // Confirm pipelineA and piplineB have different planCacheKeys.
    assert.neq(statsA[0].planCacheKey, statsB[0].planCacheKey);

    let [resultsC, statsC] = runAndGetResultsAndPlanCacheStats({pipeline: pipelineA});
    assert.eq(1, statsC.length);
    assert.eq(true, statsC[0].isActive);

    let [resultsD, statsD] = runAndGetResultsAndPlanCacheStats({pipeline: pipelineB});
    assert.eq(1, statsD.length);
    assert.eq(true, statsD[0].isActive);

    assert.eq(resultsA.length, resultsC.length);
    resultsA.every(doc => resultsC.includes(doc));
    assert.eq(resultsB.length, resultsD.length);
    resultsB.every(doc => resultsD.includes(doc));
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

    // Confirm that a different predicate with a matching shape uses the same plan cache entry.
    const stats =
        runAndGetResultsAndPlanCacheStats({distinct: "x", filter: {x: {$gt: 5}, y: 5}})[1];
    assert.eq(1, stats.length);
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

    const filter = {x: {$gt: 12}, y: {$lt: 200}};
    assertDistinctUsesPlanCacheCorrectly("x", filter);

    // Confirm that distinct on a different key uses a different plan cache entry.
    coll.getPlanCache().clear();
    const statsA = runAndGetResultsAndPlanCacheStats({distinct: "x", filter: filter})[1];
    const statsB = runAndGetResultsAndPlanCacheStats({distinct: "y", filter: filter})[1];
    assert.eq(1, statsA.length);
    assert.eq(1, statsB.length);
    assert.neq(statsA[0].planCacheKey, statsB[0].planCacheKey);

    // Confirm that find's plan cache key is unique as well.
    const findKey = getPlanCacheKeyFromExplain(coll.find(filter).explain());
    assert.neq(statsA[0].planCacheKey, findKey);
    assert.neq(statsB[0].planCacheKey, findKey);
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
    const stats = runAndGetResultsAndPlanCacheStats({
        pipeline: [
            {$match: {a: {$ne: 5}}},
            {$group: {_id: "$a", accum: {$top: {sortBy: {a: 1, b: 1}, output: "$b"}}}}
        ]
    })[1];
    assert.eq(1, stats.length);
    assert.eq(true, stats[0].isActive);

    coll.insertMany([{a: 4, b: 2, c: 3}, {a: 4, b: 3, c: 6}, {a: 5, b: 4, c: 7, d: [1, 2, 3]}]);

    // Choose DISTINCT_SCAN from plan cache over IXSCAN.
    assertAggDistinctUsesPlanCacheCorrectly(
        [{$sort: {a: -1, b: -1}}, {$group: {_id: "$a", accum: {$last: "$b"}}}]);
}

{
    coll.drop();
    coll.createIndex({a: 1, b: 1});
    coll.createIndex({b: 1, a: 1});
    coll.createIndex({a: 1, b: 1, c: 1});
    coll.createIndex({b: 1, a: 1, c: 1});

    coll.insertMany([
        {_id: 1, a: 4, b: 2, c: 3, d: 4},
        {_id: 2, a: 4, b: 3, c: 6, d: 5},
        {_id: 3, a: 5, b: 4, c: 7, d: 5}
    ]);

    // Confirm that $group pipelines with differing features have different plan cache entries.
    assert.neq(getPlanCacheKeyFromExplain(coll.explain().aggregate([{$group: {_id: "$a"}}])),
               getPlanCacheKeyFromExplain(coll.explain().aggregate([{$group: {_id: "$b"}}])));

    confirmDifferentPlanCacheEntries(
        [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$first: "$c"}}}],
        [{$sort: {a: 1, b: 1}}, {$group: {_id: "$b", accum: {$first: "$c"}}}]);

    confirmDifferentPlanCacheEntries(
        [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$first: "$c"}}}],
        [{$sort: {b: 1, a: 1}}, {$group: {_id: "$b", accum: {$first: "$c"}}}]);

    confirmDifferentPlanCacheEntries(
        [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$first: "$c"}}}],
        [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$first: "$d"}}}]);

    // Confirm that $top/$bottom with the same sort key have different plan cache entries.
    confirmDifferentPlanCacheEntries(
        [{$group: {_id: "$a", accum: {$top: {sortBy: {a: 1, b: 1}, output: "$c"}}}}],
        [{$group: {_id: "$a", accum: {$bottom: {sortBy: {a: 1, b: 1}, output: "$c"}}}}]);

    confirmDifferentPlanCacheEntries(
        [
            {$match: {a: {$gte: 4}, b: {$lt: 4}}},
            {$group: {_id: "$a", accum: {$top: {sortBy: {a: 1, b: 1}, output: "$c"}}}}
        ],
        [
            {$match: {a: {$gte: 4}, b: {$lt: 4}}},
            {$group: {_id: "$a", accum: {$bottom: {sortBy: {a: 1, b: 1}, output: "$c"}}}}
        ]);

    // Confirm that $top/$bottom with the opposite sort keys have different plan cache entries.
    confirmDifferentPlanCacheEntries(
        [{$group: {_id: "$a", accum: {$top: {sortBy: {a: 1, b: 1}, output: "$c"}}}}],
        [{$group: {_id: "$a", accum: {$bottom: {sortBy: {a: -1, b: -1}, output: "$c"}}}}]);

    // Confirm that $first/$last with the opposite sort keys have different plan cache entries.
    confirmDifferentPlanCacheEntries(
        [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$first: "$b"}}}],
        [{$sort: {a: -1, b: -1}}, {$group: {_id: "$a", accum: {$last: "$b"}}}]);

    // Confirm that $groups with different projection specs have different plan cache entries.
    confirmDifferentPlanCacheEntries(
        [{$group: {_id: "$a", accum: {$top: {sortBy: {a: 1, b: 1}, output: "$c"}}}}],
        [{$group: {_id: "$a", accum: {$top: {sortBy: {a: 1}, output: "$c"}}}}]);

    confirmDifferentPlanCacheEntries(
        [
            {$match: {a: {$gt: 0}, b: {$ne: 3}}},
            {$sort: {a: 1, b: 1}},
            {$group: {_id: "$a", accum: {$first: "$c"}}}
        ],
        [
            {$match: {a: {$gt: 0}, b: {$ne: 3}}},
            {$sort: {a: 1}},
            {$group: {_id: "$a", accum: {$first: "$c"}}}
        ]);

    // Confirm that equivalent distincts and aggregations have different plan cache entries.
    const filter = {a: {$gte: 4}, b: {$lt: 5}};
    const distinctExplain = coll.explain().distinct('a', filter);
    const pipelineExplain = coll.explain().aggregate([{$match: filter}, {$group: {_id: "$a"}}]);
    assert.neq(getPlanCacheKeyFromExplain(distinctExplain),
               getPlanCacheKeyFromExplain(pipelineExplain));
}
