/**
 * Tests DISTINCT_SCANs generated from multiplanning correctly utilize the plan cache.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan,
 * ]
 */
import {section} from "jstests/libs/pretty_md.js";
import {
    runDistinctAndOutputPlanCacheStats,
    validateAggPlanCacheUse,
    validateDistinctPlanCacheUse,
} from "jstests/libs/query/golden_test_utils.js";

const coll = db[jsTestName()];

{
    section("Distinct command utilizes plan cache");
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

    validateDistinctPlanCacheUse(coll, "x", {x: {$gt: 3}, y: 5});
}

{
    section("distinct() uses same plan cache entry with different predicate");
    coll.drop();
    for (let i = 0; i < 100; ++i) {
        coll.insert({x: i % 2, y: i + 100, z: i + 200});
    }
    coll.createIndex({x: 1});
    coll.createIndex({x: 1, y: 1});
    coll.createIndex({y: 1, z: 1});
    coll.createIndex({x: 1, y: 1, z: 1});

    runDistinctAndOutputPlanCacheStats(coll, "x", {x: {$gt: 12}, y: {$lt: 200}});
    runDistinctAndOutputPlanCacheStats(coll, "x", {x: {$gt: 12}, y: {$lt: 250}});
}

{
    section("Prefer cached IXSCAN over DISTINCT_SCAN for no duplicate values in the collection");
    coll.drop();
    for (let i = 0; i < 100; ++i)
        coll.insert({x: i, y: i + 100, z: i + 200});
    coll.createIndex({x: 1});
    coll.createIndex({x: 1, y: 1});
    coll.createIndex({y: 1, z: 1});

    validateDistinctPlanCacheUse(coll, "x", {x: {$gt: -1}, y: {$lt: 105}});
}

{
    section("Aggregation DISTINCT_SCAN utilizes plan cache");
    coll.drop();
    coll.createIndex({a: 1});
    coll.createIndex({b: 1});
    coll.createIndex({a: 1, b: 1});
    coll.createIndex({a: -1, b: 1});
    coll.createIndex({a: 1, b: -1});
    coll.createIndex({a: 1, b: 1, c: 1});
    coll.createIndex({a: 1, b: 1, d: 1});
    coll.createIndex({b: 1, a: 1});
    coll.createIndex({b: 1, c: 1});
    coll.createIndex({d: 1, c: -1});

    coll.insertMany([
        {_id: 1, a: 4, b: 2, c: 3, d: 4},
        {_id: 2, a: 4, b: 3, c: 6, d: 5},
        {_id: 3, a: 5, b: 4, c: 7, d: 5},
        {a: 4, b: 2, c: 3},
        {a: 4, b: 3, c: 6},
        {a: 5, b: 4, c: 7, d: [1, 2, 3]}
    ]);

    validateAggPlanCacheUse(coll,
                            [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$first: "$b"}}}]);

    validateAggPlanCacheUse(
        coll, [{$group: {_id: "$a", accum: {$bottom: {sortBy: {a: -1, b: -1}, output: "$c"}}}}]);

    section("DISTINCT_SCAN with embedded FETCH utilizes plan cache");

    validateAggPlanCacheUse(
        coll, [{$group: {_id: "$a", accum: {$top: {sortBy: {a: 1, b: 1}, output: "$c"}}}}]);

    validateAggPlanCacheUse(
        coll, [{$group: {_id: "$a", accum: {$bottom: {sortBy: {a: -1, b: -1}, output: "$c"}}}}]);
}
