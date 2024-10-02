/**
 * Test that the query plan cache will be cleared for the given collection on hiding or unhiding an
 * index.
 *
 * @tags: [
 *   # This test attempts to perform queries and introspect the server's plan cache entries. The
 *   # former operation may be routed to a secondary in the replica set, whereas the latter must be
 *   # routed to the primary.
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   # Plan cache state is node-local and will not get migrated alongside user data.
 *   tenant_migration_incompatible,
 *   assumes_balancer_off,
 * ]
 */

import {getPlanCacheKeyFromShape} from "jstests/libs/analyze_plan.js";

const collName = 'introspect_hidden_index_plan_cache_entries';
const collNotAffectedName = 'introspect_hidden_index_plan_cache_entries_unaffected';
db[collName].drop();
const coll = db[collName];

// This collection is used to validate that hiding an index will not affect the plan cache entries
// for other collections.
db[collNotAffectedName].drop();
const collNotAffected = db[collNotAffectedName];

function getPlansForCacheEntry(queryShape, collection) {
    const keyHash = getPlanCacheKeyFromShape({
        query: queryShape.query,
        projection: queryShape.projection,
        sort: queryShape.sort,
        collection: collection,
        db: db
    });

    const match = {
        $or: [
            {
                'createdFromQuery.query': queryShape.query,
                'createdFromQuery.sort': queryShape.sort,
                'createdFromQuery.projection': queryShape.projection
            },
            {planCacheKey: keyHash}
        ]
    };

    return collection.aggregate([{$planCacheStats: {}}, {$match: match}]).toArray();
}

const queryShape = {
    query: {a: 1},
    sort: {a: -1},
    projection: {_id: 0, a: 1}
};

function initCollection(collection) {
    assert.commandWorked(collection.insert([{a: 1, b: 1}, {a: 1, b: 2}, {a: 2, b: 2}]));

    // We need three indices so that the MultiPlanRunner will still be executed after we hide one of
    // the indexes.
    assert.commandWorked(collection.createIndex({a: 1}));
    assert.commandWorked(collection.createIndex({b: 1}));
    assert.commandWorked(collection.createIndex({a: -1, b: 1}));

    // Create a cache entry and ensure it is cached.
    const cnt = collection.find({a: 1}, {_id: 0, a: 1}).sort({a: -1}).itcount();
    assert.eq(2, cnt);
    const cachedPlan = getPlansForCacheEntry(queryShape, collection);
    assert.gt(cachedPlan.length, 0);
}

initCollection(coll);
initCollection(collNotAffected);

//
// Test that the query plan cache will be cleared for the given collection on hiding and unhiding an
// index.
//
// Hide an index.
assert.commandWorked(coll.hideIndex("b_1"));
let cachedPlan = getPlansForCacheEntry(queryShape, coll);
assert.eq(0, cachedPlan.length);

// Test that hiding an index will not affect the plan cache entries for the other collection.
cachedPlan = getPlansForCacheEntry(queryShape, collNotAffected);
assert.gt(cachedPlan.length, 0);

// Re-create the query plan.
let cnt = coll.find({a: 1}, {_id: 0, a: 1}).sort({a: -1}).itcount();
assert.eq(2, cnt);
cachedPlan = getPlansForCacheEntry(queryShape, coll);
assert.gt(cachedPlan.length, 0);

// Unhide an index.
assert.commandWorked(coll.unhideIndex("b_1"));
cachedPlan = getPlansForCacheEntry(queryShape, coll);
assert.eq(0, cachedPlan.length);
