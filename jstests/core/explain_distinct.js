// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// @tags: [
//   assumes_no_implicit_collection_creation_after_drop,
// ]

/**
 * This test ensures that explain on the distinct command works.
 */
(function() {
'use strict';

load("jstests/libs/analyze_plan.js");

const collName = "jstests_explain_distinct";
const coll = db[collName];

function runDistinctExplain(collection, keyString, query) {
    const distinctCmd = {distinct: collection.getName(), key: keyString};

    if (typeof query !== 'undefined') {
        distinctCmd.query = query;
    }

    return coll.runCommand({explain: distinctCmd, verbosity: 'executionStats'});
}

coll.drop();

// Collection doesn't exist.
let explain = runDistinctExplain(coll, 'a', {});
assert.commandWorked(explain);
let winningPlan = getWinningPlan(explain.queryPlanner);
assert(planHasStage(db, winningPlan, "EOF"));

// Insert the data to perform distinct() on.
for (let i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({a: 1, b: 1}));
    assert.commandWorked(coll.insert({a: 2, c: 1}));
}

assert.commandFailed(runDistinctExplain(coll, {}, {}));            // Bad keyString.
assert.commandFailed(runDistinctExplain(coll, 'a', 'a'));          // Bad query.
assert.commandFailed(runDistinctExplain(coll, 'b', {$not: 1}));    // Bad query.
assert.commandFailed(runDistinctExplain(coll, 'a', {$not: 1}));    // Bad query.
assert.commandFailed(runDistinctExplain(coll, '_id', {$not: 1}));  // Bad query.

// Ensure that server accepts a distinct command with no 'query' field.
assert.commandWorked(runDistinctExplain(coll, 'a', null));
assert.commandWorked(runDistinctExplain(coll, 'a'));

assert.eq([1], coll.distinct('b'));
explain = runDistinctExplain(coll, 'b', {});
assert.commandWorked(explain);
winningPlan = getWinningPlan(explain.queryPlanner);
assert.eq(20, explain.executionStats.nReturned);
assert(isCollscan(db, winningPlan));

assert.commandWorked(coll.createIndex({a: 1}));

assert.eq([1, 2], coll.distinct('a'));
explain = runDistinctExplain(coll, 'a', {});
assert.commandWorked(explain);
winningPlan = getWinningPlan(explain.queryPlanner);
assert.eq(2, explain.executionStats.nReturned);
assert(planHasStage(db, winningPlan, "PROJECTION_COVERED"));
assert(planHasStage(db, winningPlan, "DISTINCT_SCAN"));

// Check that the DISTINCT_SCAN stage has the correct stats.
let stage = getPlanStage(explain.queryPlanner.winningPlan, "DISTINCT_SCAN");
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
explain = runDistinctExplain(coll, 'a', {a: 1});
assert.commandWorked(explain);
winningPlan = getWinningPlan(explain.queryPlanner);
assert.eq(1, explain.executionStats.nReturned);
assert(planHasStage(db, winningPlan, "PROJECTION_COVERED"));
assert(planHasStage(db, winningPlan, "DISTINCT_SCAN"));

assert.eq([1], coll.distinct('b', {a: 1}));
explain = runDistinctExplain(coll, 'b', {a: 1});
assert.commandWorked(explain);
winningPlan = getWinningPlan(explain.queryPlanner);
assert.eq(1, explain.executionStats.nReturned);
assert(!planHasStage(db, winningPlan, "FETCH"));
assert(planHasStage(db, winningPlan, "PROJECTION_COVERED"));
assert(planHasStage(db, winningPlan, "DISTINCT_SCAN"));
})();
