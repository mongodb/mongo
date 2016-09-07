/**
 * This test ensures that explain on the distinct command works.
 */
(function() {
    'use strict';

    load("jstests/libs/analyze_plan.js");

    var collName = "jstests_explain_distinct";
    var coll = db[collName];

    function runDistinctExplain(collection, keyString, query) {
        var distinctCmd = {distinct: collection.getName(), key: keyString};

        if (typeof query !== 'undefined') {
            distinctCmd.query = query;
        }

        return coll.runCommand({explain: distinctCmd, verbosity: 'executionStats'});
    }

    coll.drop();

    // Collection doesn't exist.
    var explain = runDistinctExplain(coll, 'a', {});
    assert.commandWorked(explain);
    assert(planHasStage(explain.queryPlanner.winningPlan, "EOF"));

    // Insert the data to perform distinct() on.
    for (var i = 0; i < 10; i++) {
        assert.writeOK(coll.insert({a: 1, b: 1}));
        assert.writeOK(coll.insert({a: 2, c: 1}));
    }

    assert.commandFailed(runDistinctExplain(coll, {}, {}));            // Bad keyString.
    assert.commandFailed(runDistinctExplain(coll, 'a', 'a'));          // Bad query.
    assert.commandFailed(runDistinctExplain(coll, 'b', {$not: 1}));    // Bad query.
    assert.commandFailed(runDistinctExplain(coll, 'a', {$not: 1}));    // Bad query.
    assert.commandFailed(runDistinctExplain(coll, '_id', {$not: 1}));  // Bad query.

    // Ensure that server accepts a distinct command with no 'query' field.
    assert.commandWorked(runDistinctExplain(coll, '', null));
    assert.commandWorked(runDistinctExplain(coll, ''));

    assert.eq([1], coll.distinct('b'));
    var explain = runDistinctExplain(coll, 'b', {});
    assert.commandWorked(explain);
    assert.eq(20, explain.executionStats.nReturned);
    assert(isCollscan(explain.queryPlanner.winningPlan));

    assert.commandWorked(coll.createIndex({a: 1}));

    assert.eq([1, 2], coll.distinct('a'));
    var explain = runDistinctExplain(coll, 'a', {});
    assert.commandWorked(explain);
    assert.eq(2, explain.executionStats.nReturned);
    assert(planHasStage(explain.queryPlanner.winningPlan, "PROJECTION"));
    assert(planHasStage(explain.queryPlanner.winningPlan, "DISTINCT_SCAN"));

    // Check that the DISTINCT_SCAN stage has the correct stats.
    var stage = getPlanStage(explain.queryPlanner.winningPlan, "DISTINCT_SCAN");
    assert.eq({a: 1}, stage.keyPattern);
    assert.eq("a_1", stage.indexName);
    assert.eq(false, stage.isMultiKey);
    assert.eq(false, stage.isUnique);
    assert.eq(false, stage.isSparse);
    assert.eq(false, stage.isPartial);
    assert.lte(1, stage.indexVersion);
    assert("indexBounds" in stage);

    assert.commandWorked(coll.createIndex({a: 1, b: 1}));

    assert.eq([1], coll.distinct('a', {a: 1}));
    var explain = runDistinctExplain(coll, 'a', {a: 1});
    assert.commandWorked(explain);
    assert.eq(1, explain.executionStats.nReturned);
    assert(planHasStage(explain.queryPlanner.winningPlan, "PROJECTION"));
    assert(planHasStage(explain.queryPlanner.winningPlan, "DISTINCT_SCAN"));

    assert.eq([1], coll.distinct('b', {a: 1}));
    var explain = runDistinctExplain(coll, 'b', {a: 1});
    assert.commandWorked(explain);
    assert.eq(1, explain.executionStats.nReturned);
    assert(!planHasStage(explain.queryPlanner.winningPlan, "FETCH"));
    assert(planHasStage(explain.queryPlanner.winningPlan, "PROJECTION"));
    assert(planHasStage(explain.queryPlanner.winningPlan, "DISTINCT_SCAN"));
})();
