/**
 * Test that the plan cache memory estimate increases and decreases correctly as plans are added to
 * and cleared from the cache.
 */
(function() {
    "use strict";
    const conn = MongoRunner.runMongod({});
    const db = conn.getDB('test');
    const coll1 = db.query_metrics1;
    const coll2 = db.query_metrics2;
    coll1.drop();
    coll2.drop();

    const queryObj = {a: {$gte: 99}, b: -1};
    const projectionObj = {_id: 0, b: 1};
    const sortObj = {c: -1};

    function getPlanCacheSize() {
        const serverStatus = assert.commandWorked(db.serverStatus());
        return serverStatus.metrics.query.planCacheTotalSizeEstimateBytes;
    }

    function assertCacheLength(coll, length) {
        assert.eq(coll.getPlanCache().listQueryShapes().length, length);
    }

    function verifyPlanCacheSizeIncrease(coll) {
        // Add data and indices.
        for (let i = 0; i < 100; i++) {
            assert.writeOK(coll.insert({a: i, b: -1, c: 1}));
        }
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({b: 1}));

        let prevCacheSize = getPlanCacheSize();
        // Populate plan cache.
        assert.eq(1,
                  coll.find(queryObj, projectionObj).sort(sortObj).itcount(),
                  'unexpected document count');

        // Verify that the plan cache entry exists.
        assertCacheLength(coll, 1);

        // Verify that the plan cache size increased.
        assert.gt(getPlanCacheSize(), prevCacheSize);
        prevCacheSize = getPlanCacheSize();

        // Verify that the total plan cache memory consumption estimate increases when 'projection'
        // plan cache entry is added.
        assert.eq(1, coll.find(queryObj, projectionObj).itcount(), 'unexpected document count');
        assert.gt(getPlanCacheSize(), prevCacheSize);

        // Verify that the total plan cache memory consumption estimate increases when 'sort' plan
        // cache entry is added.
        prevCacheSize = getPlanCacheSize();
        assert.eq(1, coll.find(queryObj).sort(sortObj).itcount(), 'unexpected document count');
        assert.gt(getPlanCacheSize(), prevCacheSize);

        // Verify that the total plan cache memory consumption estimate increases when 'query' plan
        // cache entry is added.
        prevCacheSize = getPlanCacheSize();
        assert.eq(1, coll.find(queryObj).itcount(), 'unexpected document count');
        assert.gt(getPlanCacheSize(), prevCacheSize);
        assertCacheLength(coll, 4);
    }

    function verifyPlanCacheSizeDecrease(coll) {
        let prevCacheSize = getPlanCacheSize();
        assertCacheLength(coll, 4);

        // Verify that the total plan cache memory consumption estimate decreases when 'projection'
        // plan cache entry is cleared.
        const planCache = coll.getPlanCache();
        planCache.clearPlansByQuery(queryObj, projectionObj);
        assertCacheLength(coll, 3);
        assert.lt(getPlanCacheSize(), prevCacheSize);

        // Verify that the total plan cache memory consumption estimate decreases when 'sort' plan
        // cache entry is cleared.
        prevCacheSize = getPlanCacheSize();
        planCache.clearPlansByQuery(queryObj, undefined, sortObj);
        assertCacheLength(coll, 2);
        assert.lt(getPlanCacheSize(), prevCacheSize);

        // Verify that the total plan cache memory consumption estimate decreases when all the
        // entries for a collection are cleared.
        prevCacheSize = getPlanCacheSize();
        planCache.clear();
        assertCacheLength(coll, 0);
        assert.lt(getPlanCacheSize(), prevCacheSize);
    }

    const originalPlanCacheSize = getPlanCacheSize();

    // Verify that the cache size is zero when the database is started.
    assert.eq(originalPlanCacheSize, 0);

    // Test plan cache size estimates using multiple collections.

    // Verify that the cache size increases when entires are added.
    verifyPlanCacheSizeIncrease(coll1);

    // Verify that the cache size increases in the presence of cache from another collection.
    verifyPlanCacheSizeIncrease(coll2);

    // Verify that the cache size decreases as plans are removed from either collection.
    verifyPlanCacheSizeDecrease(coll2);
    verifyPlanCacheSizeDecrease(coll1);

    // Verify that cache size gets reset to original size after clearing all the cache entires.
    assert.eq(getPlanCacheSize(), originalPlanCacheSize);

    // Test by dropping collection.

    let coll = db.query_metrics_drop_coll;
    coll.drop();

    // Populate cache entries.
    verifyPlanCacheSizeIncrease(coll);

    // Verify that cache size gets reset to original size after dropping the collection.
    coll.drop();
    assert.eq(getPlanCacheSize(), originalPlanCacheSize);

    // Test by dropping indexes.

    coll = db.query_metrics_drop_indexes;
    coll.drop();

    // Populate cache entries.
    verifyPlanCacheSizeIncrease(coll);

    // Verify that cache size gets reset to original size after dropping indexes.
    assert.commandWorked(coll.dropIndexes());
    assert.eq(getPlanCacheSize(), originalPlanCacheSize);

    MongoRunner.stopMongod(conn);
})();
