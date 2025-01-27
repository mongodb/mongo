/**
 * Test cases that we generate an EOF plan after deriving an
 * $alwaysFalse predicate.
 *
 * @tags: [
 *   # Cannot run explain on agg queries with stepdowns
 *   does_not_support_stepdowns,
 *   # Explain for the aggregate command cannot run within a multi-document transaction.
 *   does_not_support_transactions,
 *   requires_fcv_81,
 * ]
 */

import {getWinningPlanFromExplain, isEofPlan, isIxscan} from 'jstests/libs/query/analyze_plan.js';

const coll = db[jsTestName()];
coll.drop();

/**
 * Explain both a find query and an agg pipeline with `matchPredicate` and `opts`. Pass the
 * winning plans and failureContext to `assertions`.
 */
function runAggAndFindExplainsWithAssertions(matchPredicate, opts, assertions) {
    const failureContext = "Match predicate: " + JSON.stringify(matchPredicate) +
        ". Options = " + JSON.stringify(opts);
    {
        const explain = coll.explain().aggregate([{$match: matchPredicate}], opts);
        const plan = getWinningPlanFromExplain(explain);
        assertions(plan,
                   "aggregate() assertions failed. " + failureContext +
                       ". Winning plan:" + JSON.stringify(plan));
    }
    {
        const explain = coll.find(matchPredicate, {}, opts).explain();
        const plan = getWinningPlanFromExplain(explain);
        assertions(plan,
                   "find() assertions failed. " + failureContext +
                       ". Winning plan:" + JSON.stringify(plan));
    }
}

/**
 * Assert that explaining both a find query and an agg pipeline with `matchPredicate` produce a
 * plan with an index scan node.
 */
function assertContainsEof(matchPredicate, opts = {}) {
    runAggAndFindExplainsWithAssertions(matchPredicate, opts, function(plan, failureContext) {
        assert(isEofPlan(db, plan), "Expected an EOF. " + failureContext);
        assert(!isIxscan(db, plan), "Expected no index scan. " + failureContext);
    });
}

/**
 * Assert that explaining both a find query and an agg pipeline with `matchPredicate` produce a
 * plan with an index scan node.
 */
function assertContainsIndexScan(matchPredicate, opts = {}) {
    runAggAndFindExplainsWithAssertions(matchPredicate, opts, function(plan, failureContext) {
        assert(!isEofPlan(db, plan), "Expected no EOF. " + failureContext);
        assert(isIxscan(db, plan), "Expected index scan. " + failureContext);
    });
}

assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.createIndex({x: 1, y: 1}));
assert.commandWorked(coll.createIndex({z: 1}));
assert.commandWorked(coll.insert({_id: 1, x: 1, y: 1, z: 1}));

// These are satisfiable predicates and should not generate EOF plans.
assertContainsIndexScan({x: {$in: [2]}});
assertContainsIndexScan({x: {$in: [2]}}, {hint: {x: 1}});

// Empty $in will generate $alwaysFalse and should lead to an EOF plan
// when an index on `x` is available.
assertContainsEof({x: {$in: []}});
assertContainsEof({x: {$in: []}, y: {$gt: 9}});
// No relevant index should still produce an EOF plan.
assertContainsEof({y: {$in: []}});
// Hinting an index should allow EOF plan generation.
assertContainsEof({x: {$in: []}}, {hint: {x: 1}});
assertContainsEof({x: {$in: []}, y: {$gt: 9}}, {hint: {x: 1}});
// Hinting an irrelevant index should allow EOF plan generation.
assertContainsEof({x: {$in: []}}, {hint: {z: 1}});
