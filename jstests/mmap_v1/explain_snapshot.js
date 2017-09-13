/**
 * This test ensures that .snapshot() performs an index scan on mmap_v1.
 */
(function() {
    'use strict';

    load("jstests/libs/analyze_plan.js");

    var coll = db.jstests_explain_snapshot;
    coll.drop();

    assert.writeOK(coll.insert({}));

    var explain = coll.explain().find().snapshot().finish();
    assert.commandWorked(explain);
    assert(isIxscan(explain.queryPlanner.winningPlan));

    explain = coll.find().snapshot().explain();
    assert.commandWorked(explain);
    assert(isIxscan(explain.queryPlanner.winningPlan));

    coll.drop();
})();