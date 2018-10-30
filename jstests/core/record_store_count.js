/**
 * Tests that a count will ask the record store for the count when the query predicate is empty, or
 * logically empty. See SERVER-20536 for more details.
 */

load("jstests/libs/analyze_plan.js");     // For 'planHasStage'.
load("jstests/libs/fixture_helpers.js");  // For isMongos and isSharded.

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
    // If the collection is sharded, however, then we can't use fast count, since we need to perform
    // shard filtering to avoid counting data that is not logically owned by the shard.
    //
    var explain = coll.explain().count({});
    assert(!planHasStage(db, explain.queryPlanner.winningPlan, "COLLSCAN"));
    if (!isMongos(db) || !FixtureHelpers.isSharded(coll)) {
        assert(planHasStage(db, explain.queryPlanner.winningPlan, "RECORD_STORE_FAST_COUNT"));
    }

    explain = coll.explain().count({$comment: "hi"});
    assert(!planHasStage(db, explain.queryPlanner.winningPlan, "COLLSCAN"));
    if (!isMongos(db) || !FixtureHelpers.isSharded(coll)) {
        assert(planHasStage(db, explain.queryPlanner.winningPlan, "RECORD_STORE_FAST_COUNT"));
    }

    //
    // A non-empty query predicate should prevent the use of the record store's count.
    //

    function checkPlan(plan, expectedStages, unexpectedStages) {
        for (let stage of expectedStages) {
            assert(planHasStage(db, plan, stage));
        }
        for (let stage of unexpectedStages) {
            assert(!planHasStage(db, plan, stage));
        }
    }

    function testExplainAndExpectStage({expectedStages, unexpectedStages, hintIndex}) {
        explain = coll.explain().find({x: 0}).hint(hintIndex).count();
        checkPlan(explain.queryPlanner.winningPlan, expectedStages, unexpectedStages);

        explain = coll.explain().find({x: 0, $comment: "hi"}).hint(hintIndex).count();
        checkPlan(explain.queryPlanner.winningPlan, expectedStages, unexpectedStages);
    }

    if (!isMongos(db) || !FixtureHelpers.isSharded(coll)) {
        // In an unsharded collection we can use the COUNT_SCAN stage.
        testExplainAndExpectStage(
            {expectedStages: ["COUNT_SCAN"], unexpectedStages: [], hintIndex: {x: 1}});
        return;
    }

    // The remainder of the test is only relevant for sharded clusters.

    // Without an index on the shard key, the entire document will have to be fetched.
    testExplainAndExpectStage({
        expectedStages: ["COUNT", "SHARDING_FILTER", "FETCH"],
        unexpectedStages: [],
        hintIndex: {x: 1}
    });

    // Add an index which includes the shard key. This means the FETCH should no longer be necesary
    // since the SHARDING_FILTER can get the shard key straight from the index.
    const kNewIndexSpec = {x: 1, _id: 1};
    assert.commandWorked(coll.ensureIndex(kNewIndexSpec));
    testExplainAndExpectStage({
        expectedStages: ["COUNT", "SHARDING_FILTER"],
        unexpectedStages: ["FETCH"],
        hintIndex: kNewIndexSpec
    });
})();
