// @tags: [assumes_balancer_off]
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For arrayEq().
    load("jstests/libs/analyze_plan.js");         // For getPlanStages().
    load("jstests/libs/fixture_helpers.js");      // For numberOfShardsForCollection().

    const coll = db.wildcard_nonblocking_sort;

    assert.commandWorked(coll.createIndex({"$**": 1}, {wildcardProjection: {"excludedField": 0}}));

    for (let i = 0; i < 50; i++) {
        assert.commandWorked(coll.insert({a: i, b: -i, x: [123], excludedField: i}));
    }

    function checkQueryHasSameResultsWhenUsingIdIndex(query, sort, projection) {
        const l = coll.find(query, projection).sort(sort).toArray();
        const r = coll.find(query, projection).sort(sort).hint({$natural: 1}).toArray();
        assert(arrayEq(l, r));
    }

    function checkQueryUsesSortType(query, sort, projection, isBlocking) {
        const explain = assert.commandWorked(coll.find(query, projection).sort(sort).explain());
        const plan = explain.queryPlanner.winningPlan;

        const ixScans = getPlanStages(plan, "IXSCAN");
        const sorts = getPlanStages(plan, "SORT");

        if (isBlocking) {
            assert.eq(sorts.length, FixtureHelpers.numberOfShardsForCollection(coll));
            assert.eq(sorts[0].sortPattern, sort);

            // A blocking sort may or may not use the index, so we don't check the length of
            // 'ixScans'.
        } else {
            assert.eq(sorts.length, 0);
            assert.eq(ixScans.length, FixtureHelpers.numberOfShardsForCollection(coll));

            const sortKey = Object.keys(sort)[0];
            assert.docEq(ixScans[0].keyPattern, {$_path: 1, [sortKey]: 1});
        }
    }

    function checkQueryUsesNonBlockingSortAndGetsCorrectResults(query, sort, projection) {
        checkQueryUsesSortType(query, sort, projection, false);
        checkQueryHasSameResultsWhenUsingIdIndex(query, sort, projection);
    }

    function checkQueryUsesBlockingSortAndGetsCorrectResults(query, sort, projection) {
        checkQueryUsesSortType(query, sort, projection, true);
        checkQueryHasSameResultsWhenUsingIdIndex(query, sort, projection);
    }

    function runSortTests(dir, proj) {
        // Test that the $** index can provide a non-blocking sort where appropriate.
        checkQueryUsesNonBlockingSortAndGetsCorrectResults({a: {$gte: 0}}, {a: dir}, proj);
        checkQueryUsesNonBlockingSortAndGetsCorrectResults({a: {$gte: 0}, x: 123}, {a: dir}, proj);

        // Test that the $** index can produce a solution with a blocking sort where appropriate.
        checkQueryUsesBlockingSortAndGetsCorrectResults({a: {$gte: 0}}, {a: dir, b: dir}, proj);
        checkQueryUsesBlockingSortAndGetsCorrectResults({a: {$gte: 0}}, {a: dir, b: -dir}, proj);
        checkQueryUsesBlockingSortAndGetsCorrectResults({a: {$gte: 0}}, {a: -dir, b: dir}, proj);
        checkQueryUsesBlockingSortAndGetsCorrectResults({a: {$exists: true}}, {a: dir}, proj);
        checkQueryUsesBlockingSortAndGetsCorrectResults({}, {a: dir}, proj);

        // Test sorted queries on a field that is excluded by the $** index's wildcardProjection.
        checkQueryUsesBlockingSortAndGetsCorrectResults(
            {excludedField: {$gte: 0}}, {excludedField: dir}, proj);

        // Test sorted queries on a multikey field, with and without $elemMatch.
        checkQueryUsesBlockingSortAndGetsCorrectResults({x: 123}, {a: dir}, proj);
        checkQueryUsesBlockingSortAndGetsCorrectResults(
            {x: {$elemMatch: {$eq: 123}}}, {x: dir}, proj);
        checkQueryUsesBlockingSortAndGetsCorrectResults(
            {x: {$elemMatch: {$eq: 123}}}, {a: dir}, proj);
    }

    // Run each test for both ascending and descending sorts, with and without a projection.
    for (let dir of[1, -1]) {
        for (let proj of[{}, {_id: 0, a: 1}]) {
            runSortTests(dir, proj);
        }
    }
})();
