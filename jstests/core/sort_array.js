/**
 * Tests for sorting documents by fields that contain arrays.
 */
(function() {
    "use strict";

    // The MMAP storage engine does not store path-level multikey metadata, so it cannot participate
    // in related query planning optimizations.
    const isMMAPv1 = jsTest.options().storageEngine === "mmapv1";

    load("jstests/libs/analyze_plan.js");

    let coll = db.jstests_array_sort;

    /**
     * Runs a $match-$sort-$project query as both a find and then an aggregate. Asserts that the
     * result set, after being converted to an array, is equal to 'expected'. Also asserts that the
     * find plan uses the SORT stage and the agg plan uses the "$sort" agg stage.
     */
    function testAggAndFindSort({filter, sort, project, expected}) {
        let cursor = coll.find(filter, project).sort(sort);
        assert.eq(cursor.toArray(), expected);
        let explain = coll.find(filter, project).sort(sort).explain();
        assert(planHasStage(explain.queryPlanner.winningPlan, "SORT"));

        let pipeline = [
            {$_internalInhibitOptimization: {}},
            {$match: filter},
            {$sort: sort},
            {$project: project},
        ];
        cursor = coll.aggregate(pipeline);
        assert.eq(cursor.toArray(), expected);
        explain = coll.explain().aggregate(pipeline);
        assert(aggPlanHasStage(explain, "$sort"));
    }

    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [3, 0, 1]}));
    assert.writeOK(coll.insert({_id: 1, a: [8, 4, -1]}));

    // Sanity check that a sort on "_id" is usually pushed down into the query layer, but that
    // $_internalInhibitOptimization prevents this from happening. This makes sure that this test is
    // actually exercising the agg blocking sort implementation.
    let explain = coll.explain().aggregate([{$sort: {_id: 1}}]);
    assert(!aggPlanHasStage(explain, "$sort"));
    explain = coll.explain().aggregate([{$_internalInhibitOptimization: {}}, {$sort: {_id: 1}}]);
    assert(aggPlanHasStage(explain, "$sort"));

    // Ascending sort, without an index.
    testAggAndFindSort({
        filter: {a: {$gte: 2}},
        sort: {a: 1},
        project: {_id: 1, a: 1},
        expected: [{_id: 1, a: [8, 4, -1]}, {_id: 0, a: [3, 0, 1]}]
    });

    assert.writeOK(coll.remove({}));
    assert.writeOK(coll.insert({_id: 0, a: [3, 0, 1]}));
    assert.writeOK(coll.insert({_id: 1, a: [0, 4, -1]}));

    // Descending sort, without an index.
    testAggAndFindSort({
        filter: {a: {$gte: 2}},
        sort: {a: -1},
        project: {_id: 1, a: 1},
        expected: [{_id: 1, a: [0, 4, -1]}, {_id: 0, a: [3, 0, 1]}]
    });

    assert.writeOK(coll.remove({}));
    assert.writeOK(coll.insert({_id: 0, a: [3, 0, 1]}));
    assert.writeOK(coll.insert({_id: 1, a: [8, 4, -1]}));
    assert.commandWorked(coll.createIndex({a: 1}));

    // Ascending sort, in the presence of an index. The multikey index should not be used to provide
    // the sort.
    testAggAndFindSort({
        filter: {a: {$gte: 2}},
        sort: {a: 1},
        project: {_id: 1, a: 1},
        expected: [{_id: 1, a: [8, 4, -1]}, {_id: 0, a: [3, 0, 1]}]
    });

    assert.writeOK(coll.remove({}));
    assert.writeOK(coll.insert({_id: 0, a: [3, 0, 1]}));
    assert.writeOK(coll.insert({_id: 1, a: [0, 4, -1]}));

    // Descending sort, in the presence of an index.
    testAggAndFindSort({
        filter: {a: {$gte: 2}},
        sort: {a: -1},
        project: {_id: 1, a: 1},
        expected: [{_id: 1, a: [0, 4, -1]}, {_id: 0, a: [3, 0, 1]}]
    });

    assert.writeOK(coll.remove({}));
    assert.writeOK(coll.insert({_id: 0, x: [{y: [4, 0, 1], z: 7}, {y: 0, z: 9}]}));
    assert.writeOK(coll.insert({_id: 1, x: [{y: 1, z: 7}, {y: 0, z: [8, 6]}]}));

    // Compound mixed ascending/descending sorts, without an index. Sort key for doc with _id: 0 is
    // {'': 0, '': 9}. Sort key for doc with _id: 1 is {'': 0, '': 8}.
    testAggAndFindSort({
        filter: {},
        sort: {"x.y": 1, "x.z": -1},
        project: {_id: 1},
        expected: [{_id: 0}, {_id: 1}]
    });

    // Sort key for doc with _id: 0 is {'': 4, '': 7}. Sort key for doc with _id: 1 is {'': 1, '':
    // 7}.
    testAggAndFindSort({
        filter: {},
        sort: {"x.y": -1, "x.z": 1},
        project: {_id: 1},
        expected: [{_id: 0}, {_id: 1}]
    });

    assert.commandWorked(coll.createIndex({"x.y": 1, "x.z": -1}));

    // Compound mixed ascending/descending sorts, with an index.
    testAggAndFindSort({
        filter: {},
        sort: {"x.y": 1, "x.z": -1},
        project: {_id: 1},
        expected: [{_id: 0}, {_id: 1}]
    });
    testAggAndFindSort({
        filter: {},
        sort: {"x.y": -1, "x.z": 1},
        project: {_id: 1},
        expected: [{_id: 0}, {_id: 1}]
    });

    if (!isMMAPv1) {
        // Test that, for storage engines which support path-level multikey tracking, a multikey
        // index can provide a sort over a non-multikey field.
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1, "b.c": 1}));
        assert.writeOK(coll.insert({a: [1, 2, 3], b: {c: 9}}));
        explain = coll.find({a: 2}).sort({"b.c": -1}).explain();
        assert(planHasStage(explain.queryPlanner.winningPlan, "IXSCAN"));
        assert(!planHasStage(explain.queryPlanner.winningPlan, "SORT"));

        const pipeline = [
            {$match: {a: 2}},
            {$sort: {"b.c": -1}},
        ];
        explain = coll.explain().aggregate(pipeline);
        assert(aggPlanHasStage(explain, "IXSCAN"));
        assert(!aggPlanHasStage(explain, "SORT"));
        assert(!aggPlanHasStage(explain, "$sort"));
    }

    // Test that we can correctly sort by an array field in agg when there are additional fields not
    // involved in the sort pattern.
    coll.drop();
    assert.writeOK(coll.insert(
        {_id: 0, a: 1, b: {c: 1}, d: [{e: {f: 1, g: [6, 5, 4]}}, {e: {g: [3, 2, 1]}}]}));
    assert.writeOK(coll.insert(
        {_id: 1, a: 2, b: {c: 2}, d: [{e: {f: 2, g: [5, 4, 3]}}, {e: {g: [2, 1, 0]}}]}));

    testAggAndFindSort(
        {filter: {}, sort: {"d.e.g": 1}, project: {_id: 1}, expected: [{_id: 1}, {_id: 0}]});
}());
