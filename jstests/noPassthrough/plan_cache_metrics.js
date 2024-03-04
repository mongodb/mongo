/**
 * Test that the plan cache memory estimate increases and decreases correctly as plans are added to
 * and cleared from the cache.
 * @tags: [
 *   # TODO SERVER-85728: Enable Bonsai plan cache tests involving indices.
 *   cqf_experimental_incompatible,
 * ]
 */

import {getPlanCacheSize} from "jstests/libs/plan_cache_utils.js";

const conn = MongoRunner.runMongod({});
const db = conn.getDB('test');
const coll1 = db.query_metrics1;
const coll2 = db.query_metrics2;
coll1.drop();
coll2.drop();

const queryObj = {
    a: {$gte: 99},
    b: -1
};
const projectionObj = {
    _id: 0,
    b: 1
};
const sortObj = {
    c: -1
};

function assertCacheLength(coll, length) {
    assert.eq(coll.aggregate([{$planCacheStats: {}}]).itcount(), length);
}

function verifyPlanCacheSizeIncrease(coll) {
    // Add data and indices.
    for (let i = 0; i < 100; i++) {
        assert.commandWorked(coll.insert({a: i, b: -1, c: 1}));
    }
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));

    let prevCacheSize = getPlanCacheSize(db);
    // Populate plan cache.
    assert.eq(
        1, coll.find(queryObj, projectionObj).sort(sortObj).itcount(), 'unexpected document count');

    // Verify that the plan cache entry exists.
    assertCacheLength(coll, 1);

    // Verify that the plan cache size increased.
    assert.gt(getPlanCacheSize(db), prevCacheSize);
    prevCacheSize = getPlanCacheSize(db);

    // Verify that the total plan cache memory consumption estimate increases when 'projection'
    // plan cache entry is added.
    assert.eq(1, coll.find(queryObj, projectionObj).itcount(), 'unexpected document count');
    assert.gt(getPlanCacheSize(db), prevCacheSize);

    // Verify that the total plan cache memory consumption estimate increases when 'sort' plan
    // cache entry is added.
    prevCacheSize = getPlanCacheSize(db);
    assert.eq(1, coll.find(queryObj).sort(sortObj).itcount(), 'unexpected document count');
    assert.gt(getPlanCacheSize(db), prevCacheSize);

    // Verify that the total plan cache memory consumption estimate increases when 'query' plan
    // cache entry is added.
    prevCacheSize = getPlanCacheSize(db);
    assert.eq(1, coll.find(queryObj).itcount(), 'unexpected document count');
    assert.gt(getPlanCacheSize(db), prevCacheSize);
    assertCacheLength(coll, 4);
}

function verifyPlanCacheSizeDecrease(coll) {
    let prevCacheSize = getPlanCacheSize(db);
    assertCacheLength(coll, 4);

    // Verify that the total plan cache memory consumption estimate decreases when 'projection'
    // plan cache entry is cleared.
    const planCache = coll.getPlanCache();
    planCache.clearPlansByQuery(queryObj, projectionObj);
    assertCacheLength(coll, 3);
    assert.lt(getPlanCacheSize(db), prevCacheSize);

    // Verify that the total plan cache memory consumption estimate decreases when 'sort' plan
    // cache entry is cleared.
    prevCacheSize = getPlanCacheSize(db);
    planCache.clearPlansByQuery(queryObj, undefined, sortObj);
    assertCacheLength(coll, 2);
    assert.lt(getPlanCacheSize(db), prevCacheSize);

    // Verify that the total plan cache memory consumption estimate decreases when all the
    // entries for a collection are cleared.
    prevCacheSize = getPlanCacheSize(db);
    planCache.clear();
    assertCacheLength(coll, 0);
    assert.lt(getPlanCacheSize(db), prevCacheSize);
}

const originalPlanCacheSize = getPlanCacheSize(db);

// Test plan cache size estimates using multiple collections.

// Verify that the cache size increases when entires are added.
verifyPlanCacheSizeIncrease(coll1);

// Verify that the cache size increases in the presence of cache from another collection.
verifyPlanCacheSizeIncrease(coll2);

// Verify that the cache size decreases as plans are removed from either collection.
verifyPlanCacheSizeDecrease(coll2);
verifyPlanCacheSizeDecrease(coll1);

// Verify that cache size gets reset to original size after clearing all the cache entires.
assert.eq(getPlanCacheSize(db), originalPlanCacheSize);

// Test by dropping collection.

let coll = db.query_metrics_drop_coll;
coll.drop();

// Populate cache entries.
verifyPlanCacheSizeIncrease(coll);

// Verify that cache size gets reset to original size after dropping the collection.
coll.drop();
assert.eq(getPlanCacheSize(db), originalPlanCacheSize);

// Test by dropping indexes.

coll = db.query_metrics_drop_indexes;
coll.drop();

// Populate cache entries.
verifyPlanCacheSizeIncrease(coll);

// Verify that cache size gets reset to original size after dropping indexes.
assert.commandWorked(coll.dropIndexes());
assert.eq(getPlanCacheSize(db), originalPlanCacheSize);

MongoRunner.stopMongod(conn);
