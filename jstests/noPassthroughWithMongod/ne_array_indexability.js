/**
 * Test that $ne: [] queries are cached correctly. See SERVER-39764.
 */
(function() {
    "use strict";
    load("jstests/libs/analyze_plan.js");  // For planHasStage().

    const coll = db.ne_array_indexability;
    coll.drop();

    coll.createIndex({"obj": 1});
    coll.createIndex({"obj": 1, "abc": 1});

    assert.commandWorked(coll.insert({obj: "hi there"}));

    // Check that 'queryToCache' and 'queryToRunAfterCaching' don't share a cache entry, and get
    // different plans.
    function runTest(queryToCache, queryToRunAfterCaching) {
        assert.eq(coll.find(queryToCache).itcount(), 1);
        assert.eq(coll.find(queryToCache).itcount(), 1);

        // Be sure the query has a plan in the cache.
        const res =
            coll.runCommand('planCacheListPlans', {query: queryToCache, sort: {}, projection: {}});
        assert.commandWorked(res);
        assert.gt(res.plans.length, 0);

        const explForFirstQuery = coll.find(queryToCache).explain();
        assert.eq(planHasStage(db, explForFirstQuery.queryPlanner.winningPlan, "IXSCAN"),
                  true,
                  explForFirstQuery);

        assert.eq(coll.find(queryToRunAfterCaching).itcount(), 1);

        const explForSecondQuery = coll.find(queryToRunAfterCaching).explain();
        assert.eq(planHasStage(db, explForSecondQuery.queryPlanner.winningPlan, "COLLSCAN"),
                  true,
                  explForSecondQuery);
    }

    runTest({'obj': {$ne: 'def'}}, {'obj': {$ne: [[1]]}});

    // Clear the cache.
    assert.commandWorked(coll.runCommand('planCacheClear'));

    runTest({'obj': {$nin: ['abc', 'def']}}, {'obj': {$nin: [[1], 'abc']}});
})();
