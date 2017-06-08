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
    coll.drop();

    assert.writeOK(coll.insert({_id: 0, a: [3, 0, 1]}));
    assert.writeOK(coll.insert({_id: 1, a: [0, 4, -1]}));

    // Ascending sort, without an index.
    let cursor = coll.find({a: {$gte: 2}}, {_id: 1}).sort({a: 1});
    assert.eq(cursor.next(), {_id: 1});
    assert.eq(cursor.next(), {_id: 0});
    let explain = coll.find({a: {$gte: 2}}, {_id: 1}).sort({a: 1}).explain();
    assert(planHasStage(explain.queryPlanner.winningPlan, "SORT"));

    // Descending sort, without an index.
    cursor = coll.find({a: {$gte: 2}}, {_id: 1}).sort({a: -1});
    assert.eq(cursor.next(), {_id: 1});
    assert.eq(cursor.next(), {_id: 0});
    explain = coll.find({a: {$gte: 2}}, {_id: 1}).sort({a: -1}).explain();
    assert(planHasStage(explain.queryPlanner.winningPlan, "SORT"));

    assert.commandWorked(coll.createIndex({a: 1}));

    // Ascending sort, in the presence of an index. The multikey index should not be used to provide
    // the sort.
    cursor = coll.find({a: {$gte: 2}}, {_id: 1}).sort({a: 1});
    assert.eq(cursor.next(), {_id: 1});
    assert.eq(cursor.next(), {_id: 0});
    explain = coll.find({a: {$gte: 2}}, {_id: 1}).sort({a: 1}).explain();
    assert(planHasStage(explain.queryPlanner.winningPlan, "SORT"));

    // Descending sort, in the presence of an index.
    cursor = coll.find({a: {$gte: 2}}, {_id: 1}).sort({a: -1});
    assert.eq(cursor.next(), {_id: 1});
    assert.eq(cursor.next(), {_id: 0});
    explain = coll.find({a: {$gte: 2}}, {_id: 1}).sort({a: -1}).explain();
    assert(planHasStage(explain.queryPlanner.winningPlan, "SORT"));

    assert.writeOK(coll.remove({}));
    assert.writeOK(coll.insert({_id: 0, x: [{y: [4, 0, 1], z: 7}, {y: 0, z: 9}]}));
    assert.writeOK(coll.insert({_id: 1, x: [{y: 1, z: 7}, {y: 0, z: [8, 6]}]}));

    // Compound mixed ascending/descending sorts, without an index.
    cursor = coll.find({}, {_id: 1}).sort({"x.y": 1, "x.z": -1});
    // Sort key for doc with _id: 0 is {'': 0, '': 9}.
    assert.eq(cursor.next(), {_id: 0});
    // Sort key for doc with _id: 1 is {'': 0, '': 8}.
    assert.eq(cursor.next(), {_id: 1});
    cursor = coll.find({}, {_id: 1}).sort({"x.y": 1, "x.z": -1}).explain();
    assert(planHasStage(explain.queryPlanner.winningPlan, "SORT"));

    cursor = coll.find({}, {_id: 1}).sort({"x.y": -1, "x.z": 1});
    // Sort key for doc with _id: 0 is {'': 4, '': 7}.
    assert.eq(cursor.next(), {_id: 0});
    // Sort key for doc with _id: 1 is {'': 1, '': 7}.
    assert.eq(cursor.next(), {_id: 1});
    explain = coll.find({}, {_id: 1}).sort({"x.y": -1, "x.z": 1}).explain();
    assert(planHasStage(explain.queryPlanner.winningPlan, "SORT"));

    assert.commandWorked(coll.createIndex({"x.y": 1, "x.z": -1}));

    // Compound mixed ascending/descending sorts, with an index.
    cursor = coll.find({}, {_id: 1}).sort({"x.y": 1, "x.z": -1});
    assert.eq(cursor.next(), {_id: 0});
    assert.eq(cursor.next(), {_id: 1});
    cursor = coll.find({}, {_id: 1}).sort({"x.y": 1, "x.z": -1}).explain();
    assert(planHasStage(explain.queryPlanner.winningPlan, "SORT"));

    cursor = coll.find({}, {_id: 1}).sort({"x.y": -1, "x.z": 1});
    assert.eq(cursor.next(), {_id: 0});
    assert.eq(cursor.next(), {_id: 1});
    explain = coll.find({}, {_id: 1}).sort({"x.y": -1, "x.z": 1}).explain();
    assert(planHasStage(explain.queryPlanner.winningPlan, "SORT"));

    if (!isMMAPv1) {
        // Test that, for storage engines which support path-level multikey tracking, a multikey
        // index can provide a sort over a non-multikey field.
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1, "b.c": 1}));
        assert.writeOK(coll.insert({a: [1, 2, 3], b: {c: 9}}));
        explain = coll.find({a: 2}).sort({"b.c": -1}).explain();
        assert(planHasStage(explain.queryPlanner.winningPlan, "IXSCAN"));
        assert(!planHasStage(explain.queryPlanner.winningPlan, "SORT"));
    }
}());
