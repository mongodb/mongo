// Test clearing of the plan cache, either manually through the planCacheClear command,
// or due to system events such as an index build.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: planCacheClear, reIndex.
//   not_allowed_with_signed_security_token,
//   # This test attempts to perform queries and introspect/manipulate the server's plan cache
//   # entries. The former operation may be routed to a secondary in the replica set, whereas the
//   # latter must be routed to the primary.
//   assumes_read_concern_unchanged,
//   assumes_read_preference_unchanged,
//   does_not_support_stepdowns,
//   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
//   assumes_balancer_off,
//   assumes_unsharded_collection,
//   # Plan cache state is node-local and will not get migrated alongside tenant data.
//   tenant_migration_incompatible,
//   # The SBE plan cache was first enabled in 6.3.
//   requires_fcv_63,
//   # TODO SERVER-85728: Enable Bonsai plan cache tests involving indices.
//   cqf_experimental_incompatible,
//   references_foreign_collection,
// ]

import {getPlanCacheKeyFromPipeline, getPlanCacheKeyFromShape} from "jstests/libs/analyze_plan.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

const coll = db.jstests_plan_cache_clear;
coll.drop();

function planCacheContainsQuerySet(curCache, collArg, expectedQuerySetSize) {
    const keyHashes = Array.from(curCache);
    const res =
        collArg.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: {$in: keyHashes}}}])
            .toArray();
    return (res.length == curCache.size && curCache.size == expectedQuerySetSize);
}

// Run query 'queryArg' against collection 'collArg' and add it to the map 'curCache'.
// Check that the query produced 'resCount' results, and that 'curCache' contains
// 'numCachedQueries' entries.
// Essentially, this functions runs a query, and add its both to the query cache and to
// the map 'curCache' which should mirror the queries in the query cache.
// This allows the test to keep curCache in sync with the query cache.
function addToQueryCache(
    {queryArg = {}, projectArg = {}, collArg, resCount, curCache, numCachedQueries}) {
    let keyHash = '';
    if (queryArg instanceof Array) {
        assert.eq(resCount, collArg.aggregate(queryArg).toArray().length);
        keyHash = getPlanCacheKeyFromPipeline(queryArg, collArg, db);
    } else {
        assert.eq(resCount, collArg.find(queryArg, projectArg).itcount());
        keyHash = getPlanCacheKeyFromShape(
            {query: queryArg, projection: projectArg, collection: collArg, db: db});
    }
    curCache.add(keyHash);
    assert.eq(curCache.size, numCachedQueries);
}

// Remove a query both from the query cache, and curCache.
// In this way both are kept in sync.
function deleteFromQueryCache(queryArg, collArg, curCache) {
    const beforeClearKeys =
        collArg.aggregate([{$planCacheStats: {}}, {$project: {planCacheKey: 1}}])
            .toArray()
            .map(k => k.planCacheKey);
    assert.commandWorked(collArg.runCommand('planCacheClear', {query: queryArg}));
    const afterClearKeys = collArg.aggregate([{$planCacheStats: {}}, {$project: {planCacheKey: 1}}])
                               .toArray()
                               .map(k => k.planCacheKey);
    for (let key of beforeClearKeys) {
        if (!afterClearKeys.includes(key)) {
            curCache.delete(key);
        }
    }
}

function clearQueryCaches(collArg, curCache) {
    assert.commandWorked(collArg.runCommand('planCacheClear', {}));
    curCache.clear();
}

function dumpPlanCacheState(collArg) {
    return collArg.aggregate([{$planCacheStats: {}}]).toArray();
}

assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 2, b: 2}));

// We need two indices so that the MultiPlanRunner is executed.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

// The queries in this set are expected to be in the query cache at any time.
const cachedQueries = new Set();

// Run a query so that an entry is inserted into the cache.
addToQueryCache({
    queryArg: {a: 1, b: 1},
    collArg: coll,
    resCount: 1,
    curCache: cachedQueries,
    numCachedQueries: 1
});

// Invalid key should be a no-op.
deleteFromQueryCache({unknownfield: 1}, coll, cachedQueries);
assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 1), dumpPlanCacheState(coll));

// Introduce a second plan cache entry.
addToQueryCache({
    queryArg: {a: 1, b: 1, c: 1},
    collArg: coll,
    resCount: 0,
    curCache: cachedQueries,
    numCachedQueries: 2
});
assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 2), dumpPlanCacheState(coll));

// Drop one of the two shapes from the cache.
deleteFromQueryCache({a: 1, b: 1}, coll, cachedQueries);
assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 1), dumpPlanCacheState(coll));

// Drop the second shape from the cache.
deleteFromQueryCache({a: 1, b: 1, c: 1}, coll, cachedQueries);
assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 0), dumpPlanCacheState(coll));

// planCacheClear can clear $expr queries.
addToQueryCache({
    queryArg: {a: 1, b: 1, $expr: {$eq: ['$a', 1]}},
    collArg: coll,
    resCount: 1,
    curCache: cachedQueries,
    numCachedQueries: 1
});
deleteFromQueryCache({a: 1, b: 1, $expr: {$eq: ['$a', 1]}}, coll, cachedQueries);
assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 0), dumpPlanCacheState(coll));

// planCacheClear fails with an $expr query with an unbound variable.
assert.commandFailed(
    coll.runCommand('planCacheClear', {query: {a: 1, b: 1, $expr: {$eq: ['$a', '$$unbound']}}}));

// Insert two more shapes into the cache.
addToQueryCache({
    queryArg: {a: 1, b: 1},
    collArg: coll,
    resCount: 1,
    curCache: cachedQueries,
    numCachedQueries: 1
});
addToQueryCache({
    queryArg: {a: 1, b: 1},
    projectArg: {_id: 0, a: 1},
    collArg: coll,
    resCount: 1,
    curCache: cachedQueries,
    numCachedQueries: 2
});
assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 2), dumpPlanCacheState(coll));

// Error cases.
assert.commandFailedWithCode(coll.runCommand('planCacheClear', {query: 12345}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(coll.runCommand('planCacheClear', {query: /regex/}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(coll.runCommand('planCacheClear', {query: {a: {$no_such_op: 1}}}),
                             ErrorCodes.BadValue);
// 'sort' parameter is not allowed without 'query' parameter.
assert.commandFailedWithCode(coll.runCommand('planCacheClear', {sort: {a: 1}}),
                             ErrorCodes.BadValue);
// 'projection' parameter is not allowed with 'query' parameter.
assert.commandFailedWithCode(coll.runCommand('planCacheClear', {projection: {_id: 0, a: 1}}),
                             ErrorCodes.BadValue);

// Drop query cache. This clears all cached queries in the collection.
clearQueryCaches(coll, cachedQueries);

// Clearing the plan cache for a non-existent collection should succeed.
const nonExistentColl = db.plan_cache_clear_nonexistent;
nonExistentColl.drop();
assert.commandWorked(nonExistentColl.runCommand('planCacheClear'));

if (checkSbeFullyEnabled(db)) {
    // Plan cache commands should work against the main collection only, not foreignColl
    // collections, when $lookup is pushed down into SBE.
    const foreignColl = db.plan_cache_clear_foreign;
    foreignColl.drop();
    const foreignCachedQueries = new Set();

    // We need two indices so that the multi-planner is executed.
    assert.commandWorked(foreignColl.createIndex({b: 1}));
    assert.commandWorked(foreignColl.createIndex({b: 1, c: 1}));

    assert.commandWorked(foreignColl.insert([{b: 1}, {b: 3}]));

    const pipeline = [
        {$match: {a: 1}},
        {$lookup: {from: foreignColl.getName(), localField: "a", foreignField: "b", as: "matched"}}
    ];

    // Test case 1: clear plan cache on the main collection.
    //
    // Run a query against the 'foreignColl' and make sure it's cached.
    addToQueryCache({
        queryArg: {a: 1, b: 1},
        collArg: foreignColl,
        resCount: 0,
        curCache: foreignCachedQueries,
        numCachedQueries: 1
    });
    assert.eq(true,
              planCacheContainsQuerySet(foreignCachedQueries, foreignColl, 1),
              dumpPlanCacheState(foreignColl));

    // Run the '$lookup' query and make sure it's cached.
    addToQueryCache({
        queryArg: pipeline,
        collArg: coll,
        resCount: 3,
        curCache: cachedQueries,
        numCachedQueries: 1
    });
    assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 1), dumpPlanCacheState(coll));

    // Drop query cache on the main collection. This clears all cached queries in the main
    // collection only.
    clearQueryCaches(coll, cachedQueries);
    assert.eq(true,
              planCacheContainsQuerySet(foreignCachedQueries, foreignColl, 1),
              dumpPlanCacheState(foreignColl));

    // Test case 2: clear plan cache on the foreign collection.
    //
    // Run the '$lookup' query again and make sure it's cached.
    addToQueryCache({
        queryArg: pipeline,
        collArg: coll,
        resCount: 3,
        curCache: cachedQueries,
        numCachedQueries: 1
    });
    assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 1), dumpPlanCacheState(coll));

    // Drop query cache on the foreign collection. Make sure that the plan cache on the main
    // collection is not affected.
    clearQueryCaches(foreignColl, foreignCachedQueries);
    assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 1), dumpPlanCacheState(coll));

    // Test case 3: clear plan cache on the main collection by query shape.
    //
    // Run a query against the 'foreignColl' and make sure it's cached.
    addToQueryCache({
        queryArg: {b: 1, c: 1},
        collArg: foreignColl,
        resCount: 0,
        curCache: foreignCachedQueries,
        numCachedQueries: 1
    });
    assert.eq(true,
              planCacheContainsQuerySet(foreignCachedQueries, foreignColl, 1),
              dumpPlanCacheState(foreignColl));

    // Run the '$lookup' query and make sure it's cached.
    addToQueryCache({
        queryArg: pipeline,
        collArg: coll,
        resCount: 3,
        curCache: cachedQueries,
        numCachedQueries: 1
    });
    assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 1), dumpPlanCacheState(coll));

    // Drop query cache by the query shape. This clears all cached queries in the main
    // collection only.
    deleteFromQueryCache(pipeline[0].$match, coll, cachedQueries);
    assert.eq(true,
              planCacheContainsQuerySet(foreignCachedQueries, foreignColl, 1),
              dumpPlanCacheState(foreignColl));

    // Test case 4: clear plan cache on the foreign collection by (empty) query shape.
    //
    // Run two queries against the 'foreignColl' and make sure they're cached.
    addToQueryCache({
        queryArg: {},
        collArg: foreignColl,
        resCount: 2,
        curCache: foreignCachedQueries,
        numCachedQueries: 2
    });
    addToQueryCache({
        queryArg: {b: 1, c: 1},
        collArg: foreignColl,
        resCount: 0,
        curCache: foreignCachedQueries,
        numCachedQueries: 2
    });
    assert.eq(true,
              planCacheContainsQuerySet(foreignCachedQueries, foreignColl, 2),
              dumpPlanCacheState(foreignColl));

    // Run the '$lookup' query and make sure it's cached.
    addToQueryCache({
        queryArg: pipeline,
        collArg: coll,
        resCount: 3,
        curCache: cachedQueries,
        numCachedQueries: 1
    });
    assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 1), dumpPlanCacheState(coll));

    // Drop query cache on the foreign collection by the query shape. This clears one cached
    // query in the foreign collection only.
    deleteFromQueryCache({}, foreignColl, foreignCachedQueries);
    assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 1), dumpPlanCacheState(coll));
    assert.eq(true,
              planCacheContainsQuerySet(foreignCachedQueries, foreignColl, 1),
              dumpPlanCacheState(foreignColl));

    // Test case 5: clear by query shape which matches $lookup and non-$lookup queries.
    //
    // Run the query on the main collection whose plan cache key command shape matches the shape of
    // the $lookup query.
    addToQueryCache({
        queryArg: [{$match: {a: 1}}],
        collArg: coll,
        resCount: 3,
        curCache: cachedQueries,
        numCachedQueries: 2
    });
    assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 2), dumpPlanCacheState(coll));

    // Run another query on the main collection with a totally different shape.
    addToQueryCache({
        queryArg: [{$match: {a: {$in: [1, 2]}}}],
        collArg: coll,
        resCount: 4,
        curCache: cachedQueries,
        numCachedQueries: 3
    });
    assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 3), dumpPlanCacheState(coll));

    // Drop query cache on the main collection by the query shape. This clears two cached queries in
    // the main collection which match the query shape.
    deleteFromQueryCache({a: 1}, coll, cachedQueries);
    assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 1), dumpPlanCacheState(coll));
    assert.eq(true,
              planCacheContainsQuerySet(foreignCachedQueries, foreignColl, 1),
              dumpPlanCacheState(foreignColl));
}

//
// Query Plan Revision
// http://docs.mongodb.org/manual/core/query-plans/#query-plan-revision
// As collections change over time, the query optimizer deletes the query plan and re-evaluates
// after any of the following events:
// - The reIndex rebuilds the index.
// - You add or drop an index.
// - The mongod process restarts.
//

// Make sure the cache is emtpy.
clearQueryCaches(coll, cachedQueries);

// Case 1: The reIndex rebuilds the index.
// Steps:
//     Populate the cache with 1 entry.
//     Run reIndex on the collection.
//     Confirm that cache is empty.
// (Only standalone mode supports the reIndex command.)
if (FixtureHelpers.isStandalone(db)) {
    addToQueryCache({
        queryArg: {a: 1, b: 1},
        collArg: coll,
        resCount: 1,
        curCache: cachedQueries,
        numCachedQueries: 1
    });
    assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 1), dumpPlanCacheState(coll));
    assert.commandWorked(coll.reIndex());
}

// Case 2: You add or drop an index.
// Steps:
//     Populate the cache with 1 entry.
//     Add an index.
//     Confirm that cache is empty.
clearQueryCaches(coll, cachedQueries);
addToQueryCache({
    queryArg: {a: 1, b: 1},
    collArg: coll,
    resCount: 1,
    curCache: cachedQueries,
    numCachedQueries: 1
});
assert.eq(true, planCacheContainsQuerySet(cachedQueries, coll, 1), dumpPlanCacheState(coll));
assert.commandWorked(coll.createIndex({b: 1}));
assert.eq(false, planCacheContainsQuerySet(cachedQueries, coll, 0), dumpPlanCacheState(coll));

// Case 3: The mongod process restarts
// Not applicable.
