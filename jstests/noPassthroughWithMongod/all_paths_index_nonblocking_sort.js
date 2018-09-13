(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For arrayEq().
    load("jstests/libs/analyze_plan.js");         // For getPlanStages().

    const coll = db.all_paths_nonblocking_sort;

    // Required in order to build $** indexes.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryAllowAllPathsIndexes: true}));

    assert.commandWorked(coll.createIndex({"$**": 1}, {starPathsTempName: {"excludedField": 0}}));

    for (let i = 0; i < 50; i++) {
        assert.commandWorked(coll.insert({a: i, b: -i, x: [123], excludedField: i}));
    }

    function checkQueryHasSameResultsWhenUsingIdIndex(query, sort) {
        const l = coll.find(query).sort(sort).toArray();
        const r = coll.find(query).sort(sort).hint({$natural: 1}).toArray();
        assert(arrayEq(l, r));
    }

    function checkQueryUsesSortType(query, sort, isBlocking) {
        const explain = assert.commandWorked(coll.find(query).sort(sort).explain());
        const plan = explain.queryPlanner.winningPlan;

        const ixScans = getPlanStages(plan, "IXSCAN");
        const sorts = getPlanStages(plan, "SORT");

        if (isBlocking) {
            assert.eq(sorts.length, 1);
            assert.eq(sorts[0].sortPattern, sort);

            // A blocking sort may or may not use the index, so we don't check the length of
            // 'ixScans'.
        } else {
            assert.eq(sorts.length, 0);
            assert.eq(ixScans.length, 1);

            const sortKey = Object.keys(sort)[0];
            assert.docEq(ixScans[0].keyPattern, {$_path: 1, [sortKey]: 1});
        }
    }

    function checkQueryUsesNonBlockingSortAndGetsCorrectResults(query, sort) {
        checkQueryUsesSortType(query, sort, false);
        checkQueryHasSameResultsWhenUsingIdIndex(query, sort);
    }

    function checkQueryUsesBlockingSortAndGetsCorrectResults(query, sort) {
        checkQueryUsesSortType(query, sort, true);
        checkQueryHasSameResultsWhenUsingIdIndex(query, sort);
    }

    checkQueryUsesNonBlockingSortAndGetsCorrectResults({a: {$gte: 0}}, {a: 1});
    checkQueryUsesNonBlockingSortAndGetsCorrectResults({a: {$gte: 0}, x: 123}, {a: 1});

    checkQueryUsesBlockingSortAndGetsCorrectResults({x: {$elemMatch: {$eq: 123}}}, {x: 1});
    checkQueryUsesBlockingSortAndGetsCorrectResults({x: {$elemMatch: {$eq: 123}}}, {a: 1});
    checkQueryUsesBlockingSortAndGetsCorrectResults({a: {$gte: 0}}, {a: 1, b: 1});
    checkQueryUsesBlockingSortAndGetsCorrectResults({a: {$exists: true}}, {a: 1});
    checkQueryUsesBlockingSortAndGetsCorrectResults({}, {a: 1});
    checkQueryUsesBlockingSortAndGetsCorrectResults({x: 123}, {a: 1});
    checkQueryUsesBlockingSortAndGetsCorrectResults({excludedField: {$gte: 0}}, {excludedField: 1});

    // TODO SERVER-36444: Remove drop once collection validation works with indexes that have
    // multikey entries.
    coll.drop();
})();
