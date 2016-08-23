(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");

    var t = db.distinct_multikey_index;

    t.drop();
    for (var i = 0; i < 10; i++) {
        t.save({a: 1, b: 1});
        t.save({a: 1, b: 2});
        t.save({a: 2, b: 1});
        t.save({a: 2, b: 3});
    }
    t.createIndex({a: 1, b: 1});

    var explain = t.distinct('b', {a: 1}).explain("executionStats");
    assert.commandWorked(explain);
    assert(planHasStage(explain.queryPlanner.winningPlan, "DISTINCT_SCAN"));
    assert(planHasState(explain.queryPlanner.winningPlan, "PROJECTION"));
    assert.eq(2, explain.executionStats.nReturned);

    var stage = getPlanStage(explain.queryPlanner.winningPlan, "DISTINCT_SCAN");
    assert.eq({a: 1, b: 1}, stage.keyPattern);
    assert.eq("a_1_b_1", stage.indexName);
    assert.eq(false, stage.isMultiKey);
    assert.eq(false, stage.isUnique);
    assert.eq(false, stage.isSparse);
    assert.eq(false, stage.isPartial);
    assert.eq(1, stage.indexVersion);
    assert("indexBounds" in stage);
})();
