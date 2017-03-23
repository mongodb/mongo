// Cannot implicitly shard accessed collections because queries on a sharded collection are not
// able to be covered when they aren't on the shard key since the document needs to be fetched in
// order to apply the SHARDING_FILTER stage.
// @tags: [assumes_unsharded_collection]

/**
 * Test covering behavior for queries over a multikey index.
 */
(function() {
    "use strict";

    // The MMAP storage engine does not store path-level multikey metadata, so it cannot participate
    // in related query planning optimizations.
    const isMMAPv1 = jsTest.options().storageEngine === "mmapv1";

    // For making assertions about explain output.
    load("jstests/libs/analyze_plan.js");

    let coll = db.covered_multikey;
    coll.drop();

    assert.writeOK(coll.insert({a: 1, b: [2, 3, 4]}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));

    assert.eq(1, coll.find({a: 1, b: 2}, {_id: 0, a: 1}).itcount());
    assert.eq({a: 1}, coll.findOne({a: 1, b: 2}, {_id: 0, a: 1}));
    let explainRes = coll.explain("queryPlanner").find({a: 1, b: 2}, {_id: 0, a: 1}).finish();
    assert(isIxscan(explainRes.queryPlanner.winningPlan));
    if (isMMAPv1) {
        assert(planHasStage(explainRes.queryPlanner.winningPlan, "FETCH"));
    } else {
        assert(!planHasStage(explainRes.queryPlanner.winningPlan, "FETCH"));
    }

    coll.drop();
    assert.writeOK(coll.insert({a: 1, b: [1, 2, 3], c: 3, d: 5}));
    assert.writeOK(coll.insert({a: [1, 2, 3], b: 1, c: 4, d: 6}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1, c: -1, d: -1}));

    let cursor = coll.find({a: 1, b: 1}, {_id: 0, c: 1, d: 1}).sort({c: -1, d: -1});
    assert.eq(cursor.next(), {c: 4, d: 6});
    assert.eq(cursor.next(), {c: 3, d: 5});
    assert(!cursor.hasNext());
    explainRes = coll.explain("queryPlanner")
                     .find({a: 1, b: 1}, {_id: 0, c: 1, d: 1})
                     .sort({c: -1, d: -1})
                     .finish();
    if (isMMAPv1) {
        assert(planHasStage(explainRes.queryPlanner.winningPlan, "FETCH"));
    } else {
        assert(!planHasStage(explainRes.queryPlanner.winningPlan, "FETCH"));
    }

    // Verify that a query cannot be covered over a path which is multikey due to an empty array.
    coll.drop();
    assert.writeOK(coll.insert({a: []}));
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.eq({a: []}, coll.findOne({a: []}, {_id: 0, a: 1}));
    explainRes = coll.explain("queryPlanner").find({a: []}, {_id: 0, a: 1}).finish();
    assert(planHasStage(explainRes.queryPlanner.winningPlan, "IXSCAN"));
    assert(planHasStage(explainRes.queryPlanner.winningPlan, "FETCH"));
    let ixscanStage = getPlanStage(explainRes.queryPlanner.winningPlan, "IXSCAN");
    assert.eq(true, ixscanStage.isMultiKey);

    // Verify that a query cannot be covered over a path which is multikey due to a single-element
    // array.
    coll.drop();
    assert.writeOK(coll.insert({a: [2]}));
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.eq({a: [2]}, coll.findOne({a: 2}, {_id: 0, a: 1}));
    explainRes = coll.explain("queryPlanner").find({a: 2}, {_id: 0, a: 1}).finish();
    assert(planHasStage(explainRes.queryPlanner.winningPlan, "IXSCAN"));
    assert(planHasStage(explainRes.queryPlanner.winningPlan, "FETCH"));
    ixscanStage = getPlanStage(explainRes.queryPlanner.winningPlan, "IXSCAN");
    assert.eq(true, ixscanStage.isMultiKey);

    // Verify that a query cannot be covered over a path which is multikey due to a single-element
    // array, where the path is made multikey by an update rather than an insert.
    coll.drop();
    assert.writeOK(coll.insert({a: 2}));
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.writeOK(coll.update({}, {$set: {a: [2]}}));
    assert.eq({a: [2]}, coll.findOne({a: 2}, {_id: 0, a: 1}));
    explainRes = coll.explain("queryPlanner").find({a: 2}, {_id: 0, a: 1}).finish();
    assert(planHasStage(explainRes.queryPlanner.winningPlan, "IXSCAN"));
    assert(planHasStage(explainRes.queryPlanner.winningPlan, "FETCH"));
    ixscanStage = getPlanStage(explainRes.queryPlanner.winningPlan, "IXSCAN");
    assert.eq(true, ixscanStage.isMultiKey);

    // Verify that a trailing empty array makes a 2dsphere index multikey.
    coll.drop();
    assert.commandWorked(coll.createIndex({"a.b": 1, c: "2dsphere"}));
    assert.writeOK(coll.insert({a: {b: 1}, c: {type: "Point", coordinates: [0, 0]}}));
    explainRes = coll.explain().find().hint({"a.b": 1, c: "2dsphere"}).finish();
    ixscanStage = getPlanStage(explainRes.queryPlanner.winningPlan, "IXSCAN");
    assert.neq(null, ixscanStage);
    assert.eq(false, ixscanStage.isMultiKey);
    assert.writeOK(coll.insert({a: {b: []}, c: {type: "Point", coordinates: [0, 0]}}));
    explainRes = coll.explain().find().hint({"a.b": 1, c: "2dsphere"}).finish();
    ixscanStage = getPlanStage(explainRes.queryPlanner.winningPlan, "IXSCAN");
    assert.neq(null, ixscanStage);
    assert.eq(true, ixscanStage.isMultiKey);

    // Verify that a mid-path empty array makes a 2dsphere index multikey.
    coll.drop();
    assert.commandWorked(coll.createIndex({"a.b": 1, c: "2dsphere"}));
    assert.writeOK(coll.insert({a: [], c: {type: "Point", coordinates: [0, 0]}}));
    explainRes = coll.explain().find().hint({"a.b": 1, c: "2dsphere"}).finish();
    ixscanStage = getPlanStage(explainRes.queryPlanner.winningPlan, "IXSCAN");
    assert.neq(null, ixscanStage);
    assert.eq(true, ixscanStage.isMultiKey);

    // Verify that a single-element array makes a 2dsphere index multikey.
    coll.drop();
    assert.commandWorked(coll.createIndex({"a.b": 1, c: "2dsphere"}));
    assert.writeOK(coll.insert({a: {b: [3]}, c: {type: "Point", coordinates: [0, 0]}}));
    explainRes = coll.explain().find().hint({"a.b": 1, c: "2dsphere"}).finish();
    ixscanStage = getPlanStage(explainRes.queryPlanner.winningPlan, "IXSCAN");
    assert.neq(null, ixscanStage);
    assert.eq(true, ixscanStage.isMultiKey);
}());
