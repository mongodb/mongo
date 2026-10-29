/**
 * Test that index filter commands (planCacheSetFilter, planCacheClearFilters) invalidate the
 * corresponding plan cache entries.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: planCacheClearFilters,
 *   # planCacheSetFilter.
 *   not_allowed_with_signed_security_token,
 *   # This test attempts to perform queries with plan cache filters set up. The index filter
 *   # commands and the queries to which those index filters apply could be routed to different
 *   # nodes.
 *   assumes_read_preference_unchanged,
 *   assumes_read_concern_unchanged,
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 *   requires_fcv_62,
 *   # Plan cache state is node-local and will not get migrated alongside user data
 *   assumes_balancer_off,
 * ]
 */
import {getPlanCacheKeyFromShape} from "jstests/libs/query/analyze_plan.js";
const collName = "index_filter_commands_invalidate_plan_cache_entries";
const coll = db[collName];

function initCollection(collection) {
    collection.drop();

    // We need multiple indexes so that the multi-planner is executed.
    assert.commandWorked(collection.createIndex({a: 1}));
    assert.commandWorked(collection.createIndex({b: 1}));
    assert.commandWorked(collection.createIndex({a: -1, b: 1}));
    assert.commandWorked(collection.insert({a: 1, b: 1, c: 1}));
}
initCollection(coll);

function existsInPlanCache(query, sort, projection, planCacheColl) {
    const keyHash = getPlanCacheKeyFromShape({
        query: query,
        projection: projection,
        sort: sort,
        collection: planCacheColl,
        db: db,
    });
    const res = planCacheColl
        .aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: keyHash}}])
        .toArray();

    return res.length > 0;
}

assert.eq(1, coll.find({a: 1}).itcount());
assert(existsInPlanCache({a: 1}, {}, {}, coll));
assert.eq(1, coll.find({a: 1, b: 1}).itcount());
assert(existsInPlanCache({a: 1, b: 1}, {}, {}, coll));
assert.eq(1, coll.find({a: 1, b: 1}).sort({a: 1}).itcount());
assert(existsInPlanCache({a: 1, b: 1}, {a: 1}, {}, coll));

// We check that the plan cache contains at least 3 entries because we can't control how many extra
// plans are created to perform the queries being run in parallel by other concurrent tasks.
assert.gte(coll.aggregate([{$planCacheStats: {}}]).toArray().length, 3);

assert.commandWorked(
    db.runCommand({planCacheSetFilter: collName, query: {a: 1, b: 1}, indexes: [{a: 1}]}),
);
assert(!existsInPlanCache({a: 1, b: 1}, {}, {}, coll));

// This planCacheSetFilter command will invalidate plan cache entries with filter {a: 1}.
assert.commandWorked(
    db.runCommand({planCacheSetFilter: collName, query: {a: 1}, indexes: [{a: 1}]}),
);
assert(!existsInPlanCache({a: 1}, {}, {}, coll));

// Test that plan cache entries with same query shape but in a different collection won't be cleared
// when an index filter with the same query shape is set/cleared.
const collNameOther = "index_filter_commands_invalidate_plan_cache_entries_other";
const collOther = db[collNameOther];

initCollection(coll);
initCollection(collOther);

assert.eq(1, coll.find({a: 1, b: 1}).itcount());
assert(existsInPlanCache({a: 1, b: 1}, {}, {}, coll));
assert.eq(1, collOther.find({a: 1, b: 1}).itcount());
assert(existsInPlanCache({a: 1, b: 1}, {}, {}, collOther));

// Test that planCacheClearFilters command invalidates corresponding plan cache entries of correct
// collection.
assert.commandWorked(db.runCommand({planCacheClearFilters: collName, query: {a: 1, b: 1}}));
assert(!existsInPlanCache({a: 1, b: 1}, {}, {}, coll));
assert(existsInPlanCache({a: 1, b: 1}, {}, {}, collOther));

// Test planCacheSetFilter command invalidates corresponding plan cache entries of correct
// collection.
assert.eq(1, coll.find({a: 1, b: 1}).itcount());
assert(existsInPlanCache({a: 1, b: 1}, {}, {}, coll));

assert.commandWorked(
    db.runCommand({planCacheSetFilter: collName, query: {a: 1, b: 1}, indexes: [{a: 1}]}),
);
assert(!existsInPlanCache({a: 1, b: 1}, {}, {}, coll));
assert(existsInPlanCache({a: 1, b: 1}, {}, {}, collOther));
