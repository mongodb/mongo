/**
 * Tests that detailed debug information is excluded from new plan cache entries once the estimated
 * cumulative size of the system's plan caches exceeds a pre-configured threshold.
 */
(function() {
    "use strict";

    /**
     * Creates two indexes for the given collection. In order for plans to be cached, there need to
     * be at least two possible indexed plans.
     */
    function createIndexesForColl(coll) {
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({b: 1}));
    }

    function totalPlanCacheSize() {
        const serverStatus = assert.commandWorked(db.serverStatus());
        return serverStatus.metrics.query.planCacheTotalSizeEstimateBytes;
    }

    function planCacheContents(coll) {
        const result =
            assert.commandWorked(db.runCommand({planCacheListQueryShapes: coll.getName()}));
        assert(result.hasOwnProperty("shapes"), tojson(result));
        return result.shapes;
    }

    /**
     * Retrieve the cache entry associated with the query shape defined by the given 'filter'
     * (assuming the query has no projection, sort, or collation) using the 'planCacheListPlans'
     * command.  Asserts that the plan cache entry exists, and returns it.
     */
    function getPlanCacheEntryForFilter(coll, filter) {
        const cmdResult = assert.commandWorked(
            db.runCommand({planCacheListPlans: coll.getName(), query: filter}));
        // Ensure that an entry actually exists in the cache for this query shape.
        assert.gt(cmdResult.plans.length, 0, tojson(cmdResult));
        return cmdResult;
    }

    function assertExistenceOfRequiredCacheEntryFields(entry) {
        assert(entry.hasOwnProperty("works"), tojson(entry));
        assert(entry.hasOwnProperty("timeOfCreation"), tojson(entry));
        assert(entry.hasOwnProperty("estimatedSizeBytes"), tojson(entry));

        assert(entry.hasOwnProperty("plans"), tojson(entry));
        for (let plan of entry.plans) {
            assert(plan.hasOwnProperty("details"), tojson(plan));
            assert(plan.details.hasOwnProperty("solution"), tojson(plan));
            assert(plan.hasOwnProperty("filterSet"), tojson(plan));
        }
    }

    function assertCacheEntryHasDebugInfo(entry) {
        assertExistenceOfRequiredCacheEntryFields(entry);

        // Check that fields deriving from debug info are present.
        for (let i = 0; i < entry.plans.length; ++i) {
            const plan = entry.plans[i];
            assert(plan.hasOwnProperty("reason"), tojson(plan));
            assert(plan.reason.hasOwnProperty("score"), tojson(plan));
            assert(plan.reason.hasOwnProperty("stats"), tojson(plan));
        }
        assert(entry.plans[0].hasOwnProperty("feedback"), tojson(entry));
        assert(entry.plans[0].feedback.hasOwnProperty("nfeedback"), tojson(entry));
        assert(entry.plans[0].feedback.hasOwnProperty("scores"), tojson(entry));
    }

    function assertCacheEntryIsMissingDebugInfo(entry) {
        assertExistenceOfRequiredCacheEntryFields(entry);

        // Verify that fields deriving from debug info  are missing for the legacy format.
        for (let i = 0; i < entry.plans.length; ++i) {
            const plan = entry.plans[i];
            assert(!plan.hasOwnProperty("reason"), tojson(plan));
            assert(!plan.hasOwnProperty("feedback"), tojson(plan));
        }

        // We expect cache entries to be reasonably small when their debug info is stripped.
        // Although there are no strict guarantees on the size of the entry, we can expect that the
        // size estimate should always remain under 4kb.
        assert.lt(entry.estimatedSizeBytes, 4 * 1024, tojson(entry));
    }

    /**
     * Given a match expression 'filter' describing a query shape, obtains the associated plan cache
     * information using 'planCacheListPlans'. Verifies that the plan cache entry exists and
     * contains the expected debug info.
     */
    function assertQueryShapeHasDebugInfoInCache(coll, filter) {
        const cacheEntry = getPlanCacheEntryForFilter(coll, filter);
        assertCacheEntryHasDebugInfo(cacheEntry);
    }

    /**
     * Given a match expression 'filter' describing a query shape, obtains the associated plan cache
     * information using 'planCacheListPlans'. Verifies that the plan cache entry exists but has had
     * its debug info stripped.
     */
    function assertQueryShapeIsMissingDebugInfoInCache(coll, filter) {
        const cacheEntry = getPlanCacheEntryForFilter(coll, filter);
        assertCacheEntryIsMissingDebugInfo(cacheEntry);
    }

    /**
     * Given a previous total plan cache size, 'oldCacheSize', and a newer observation of the plan
     * cache size, 'newCacheSize', asserts that this growth is consistent with the size reported by
     * 'cacheEntry'.
     */
    function assertChangeInCacheSizeIsDueToEntry(cacheEntry, oldCacheSize, newCacheSize) {
        const cacheSizeGrowth = newCacheSize - oldCacheSize;
        // Instead of asserting that the cache size growth is exactly equal to the cache entry's
        // size, we assert that the difference between them is sufficiently small. This wiggle room
        // is necessary since the reported cache size is an estimate and may not be precise.
        assert.lt(Math.abs(cacheEntry.estimatedSizeBytes - cacheSizeGrowth), 50);
    }

    const conn = MongoRunner.runMongod({});
    assert.neq(conn, null, "mongod failed to start");
    const db = conn.getDB("test");
    const coll = db.plan_cache_memory_debug_info;
    coll.drop();
    createIndexesForColl(coll);

    const smallQuery = {
        a: 1,
        b: 1,
    };

    // Create a plan cache entry, and verify that the estimated plan cache size has increased.
    let oldPlanCacheSize = totalPlanCacheSize();
    assert.eq(0, coll.find(smallQuery).itcount());
    let newPlanCacheSize = totalPlanCacheSize();
    assert.gt(newPlanCacheSize, oldPlanCacheSize);

    // Verify that the cache now has a single entry whose estimated size explains the increase in
    // the total plan cache size reported by serverStatus(). The cache entry should contain all
    // expected debug info.
    let cacheContents = planCacheContents(coll);
    assert.eq(cacheContents.length, 1, cacheContents);
    const cacheEntry = getPlanCacheEntryForFilter(coll, cacheContents[0].query);
    assertCacheEntryHasDebugInfo(cacheEntry);
    assertQueryShapeHasDebugInfoInCache(coll, smallQuery);
    assertChangeInCacheSizeIsDueToEntry(cacheEntry, oldPlanCacheSize, newPlanCacheSize);

    // Configure the server so that new plan cache entries should not preserve debug info.
    const setParamRes = assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryCacheMaxSizeBytesBeforeStripDebugInfo: 0}));
    const stripDebugInfoThresholdDefault = setParamRes.was;

    // Generate a query which includes a 10,000 element $in predicate.
    const kNumInElements = 10 * 1000;
    const largeQuery = {
        a: 1,
        b: 1,
        c: {$in: Array.from({length: kNumInElements}, (_, i) => i)},
    };

    // Create a new cache entry using the query with the large $in predicate. Verify that the
    // estimated total plan cache size has increased again, and check that there are now two entries
    // in the cache.
    oldPlanCacheSize = totalPlanCacheSize();
    assert.eq(0, coll.find(largeQuery).itcount());
    newPlanCacheSize = totalPlanCacheSize();
    assert.gt(newPlanCacheSize, oldPlanCacheSize);
    cacheContents = planCacheContents(coll);
    assert.eq(cacheContents.length, 2, cacheContents);

    // The cache entry associated with 'smallQuery' should retain its debug info, whereas the cache
    // entry associated with 'largeQuery' should have had its debug info stripped.
    assertQueryShapeHasDebugInfoInCache(coll, smallQuery);
    assertQueryShapeIsMissingDebugInfoInCache(coll, largeQuery);

    // The second cache entry should be smaller than the first, despite the query being much larger.
    const smallQueryCacheEntry = getPlanCacheEntryForFilter(coll, smallQuery);
    let largeQueryCacheEntry = getPlanCacheEntryForFilter(coll, largeQuery);
    assert.lt(largeQueryCacheEntry.estimatedSizeBytes,
              smallQueryCacheEntry.estimatedSizeBytes,
              cacheContents);

    // The new cache entry's size should account for the latest observed increase in total plan
    // cache size.
    assertChangeInCacheSizeIsDueToEntry(largeQueryCacheEntry, oldPlanCacheSize, newPlanCacheSize);

    // Verify that a new cache entry in a different collection also has its debug info stripped.
    // This demonstrates that the size threshold applies on a server-wide basis as opposed to on a
    // per-collection basis.
    const secondColl = db.plan_cache_memory_debug_info_other;
    secondColl.drop();
    createIndexesForColl(secondColl);

    // Introduce a new cache entry in the second collection's cache and verify that the cumulative
    // plan cache size has increased.
    oldPlanCacheSize = totalPlanCacheSize();
    assert.eq(0, secondColl.find(smallQuery).itcount());
    newPlanCacheSize = totalPlanCacheSize();
    assert.gt(newPlanCacheSize, oldPlanCacheSize);

    // Ensure that the second collection's cache now has one entry, and that entry's debug info is
    // stripped.
    cacheContents = planCacheContents(secondColl);
    assert.eq(cacheContents.length, 1, cacheContents);
    assertQueryShapeIsMissingDebugInfoInCache(secondColl, smallQuery);

    // Meanwhile, the contents of the original collection's plan cache should remain unchanged.
    cacheContents = planCacheContents(coll);
    assert.eq(cacheContents.length, 2, cacheContents);
    assertQueryShapeHasDebugInfoInCache(coll, smallQuery);
    assertQueryShapeIsMissingDebugInfoInCache(coll, largeQuery);

    // Ensure that 'planCacheListQueryShapes' works when debug info has been stripped. For a cache
    // entry which is missing debug info, we expect 'planCacheListQueryShapes' to display an empty
    // object.
    const listQueryShapesResult =
        assert.commandWorked(db.runCommand({planCacheListQueryShapes: secondColl.getName()}));
    assert(listQueryShapesResult.hasOwnProperty("shapes"), tojson(listQueryShapesResult));
    assert.eq(listQueryShapesResult.shapes.length, 1, listQueryShapesResult);
    const listedShape = listQueryShapesResult.shapes[0];
    assert.eq(listedShape, {}, listQueryShapesResult);

    // Restore the threshold for stripping debug info to its default. Verify that if we add a third
    // cache entry to the original collection 'coll', the plan cache size increases once again, and
    // the new cache entry stores debug info.
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryCacheMaxSizeBytesBeforeStripDebugInfo: stripDebugInfoThresholdDefault,
    }));
    const smallQuery2 = {
        a: 1,
        b: 1,
        c: 1,
    };
    oldPlanCacheSize = totalPlanCacheSize();
    assert.eq(0, coll.find(smallQuery2).itcount());
    newPlanCacheSize = totalPlanCacheSize();
    assert.gt(newPlanCacheSize, oldPlanCacheSize);

    // Verify that there are now three cache entries.
    cacheContents = planCacheContents(coll);
    assert.eq(cacheContents.length, 3, cacheContents);

    // Make sure that the cache entries have or are missing debug info as expected.
    assertQueryShapeHasDebugInfoInCache(coll, smallQuery);
    assertQueryShapeHasDebugInfoInCache(coll, smallQuery2);
    assertQueryShapeIsMissingDebugInfoInCache(coll, largeQuery);
    assertQueryShapeIsMissingDebugInfoInCache(secondColl, smallQuery);

    // Clear the cache entry for 'largeQuery' and regenerate it. The cache should grow larger, since
    // the regenerated cache entry should now contain debug info. Also, check that the size of the
    // new cache entry is estimated to be at least 10kb, since the query itself is known to be at
    // least 10kb.
    oldPlanCacheSize = totalPlanCacheSize();
    assert.commandWorked(coll.runCommand("planCacheClear", {query: largeQuery}));
    cacheContents = planCacheContents(coll);
    assert.eq(cacheContents.length, 2, cacheContents);

    assert.eq(0, coll.find(largeQuery).itcount());
    cacheContents = planCacheContents(coll);
    assert.eq(cacheContents.length, 3, cacheContents);

    newPlanCacheSize = totalPlanCacheSize();
    assert.gt(newPlanCacheSize, oldPlanCacheSize);

    assertQueryShapeHasDebugInfoInCache(coll, largeQuery);
    largeQueryCacheEntry = getPlanCacheEntryForFilter(coll, largeQuery);
    assert.gt(largeQueryCacheEntry.estimatedSizeBytes, 10 * 1024, largeQueryCacheEntry);

    MongoRunner.stopMongod(conn);
}());
