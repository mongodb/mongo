/**
 * Test queries over a multikey index that can be covered.
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
}());
