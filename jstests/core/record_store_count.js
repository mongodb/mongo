/**
 * Tests that a count will ask the record store for the count when the query predicate is empty, or
 * logically empty. See SERVER-20536 for more details.
 */

load("jstests/libs/analyze_plan.js");  // For 'planHasStage'.

(function() {
    "use strict";

    var coll = db.record_store_count;
    coll.drop();

    assert.writeOK(coll.insert({x: 0}));
    assert.writeOK(coll.insert({x: 1}));

    assert.commandWorked(coll.ensureIndex({x: 1}));

    //
    // Logically empty predicates should use the record store's count.
    //
    var explain = coll.explain().count({});
    assert(!planHasStage(explain.queryPlanner.winningPlan, "COLLSCAN"));

    explain = coll.explain().count({$comment: "hi"});
    assert(!planHasStage(explain.queryPlanner.winningPlan, "COLLSCAN"));

    //
    // A non-empty query predicate should prevent the use of the record store's count.
    //
    explain = coll.explain().find({x: 0}).count();
    assert(planHasStage(explain.queryPlanner.winningPlan, "COUNT_SCAN"));

    explain = coll.explain().find({x: 0, $comment: "hi"}).count();
    assert(planHasStage(explain.queryPlanner.winningPlan, "COUNT_SCAN"));

    explain = coll.explain().find({x: 0}).hint({x: 1}).count();
    assert(planHasStage(explain.queryPlanner.winningPlan, "COUNT_SCAN"));

    explain = coll.explain().find({x: 0, $comment: "hi"}).hint({x: 1}).count();
    assert(planHasStage(explain.queryPlanner.winningPlan, "COUNT_SCAN"));
})();
