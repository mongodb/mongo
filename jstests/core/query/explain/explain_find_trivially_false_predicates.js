/**
 * Tests for optimizations applied to trivially false predicates.
 * @tags: [
 *   requires_fcv_81,
 *   # Explain command does not support read concerns other than local
 *   assumes_read_concern_local
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    assertNoFetchFilter,
    getAggPlanStage,
    getPlanStages,
    getWinningPlanFromExplain,
    isEofPlan,
} from "jstests/libs/query/analyze_plan.js";

const collName = "jstests_explain_find_trivially_false_predicates";

[
    {description: "Regular collections", collOptions: {}},
    {
        description: "Clustered collections",
        collOptions: {clusteredIndex: {key: {_id: 1}, unique: true, name: "Clustered index definition"}},
    },
].forEach((testConfig) => {
    jsTestLog(`Testing trivially false optimization with ${testConfig.description}`);
    assertDropAndRecreateCollection(db, collName, testConfig.collOptions);
    const coll = db[collName];

    assert.commandWorked(coll.insert(Array.from({length: 10}, (_, i) => ({_id: i, a: i}))));

    // Finding something trivially false (e.g: alwaysFalse) is optimized using an EOF plan.
    let explain = coll.find({$alwaysFalse: 1}).explain();
    let winningPlan = getWinningPlanFromExplain(explain);
    assert(isEofPlan(db, winningPlan));
    let eofStages = getPlanStages(winningPlan, "EOF");
    eofStages.forEach((stage) => assert.eq(stage.type, "predicateEvalsToFalse"));

    // It also uses EOF for queries including projection, sorting, limit and skip arguments.
    explain = coll.find({$alwaysFalse: 1}, {_id: 0, a: 1}).skip(1).limit(2).explain();
    winningPlan = getWinningPlanFromExplain(explain);
    assert(isEofPlan(db, winningPlan));
    eofStages = getPlanStages(winningPlan, "EOF");
    eofStages.forEach((stage) => assert.eq(stage.type, "predicateEvalsToFalse"));

    explain = coll.find({$nor: [{$alwaysTrue: 1}]}).explain();
    winningPlan = getWinningPlanFromExplain(explain);
    assert(isEofPlan(db, winningPlan));

    explain = coll.find({$nor: [{$nor: [{$alwaysFalse: 1}]}]}).explain();
    winningPlan = getWinningPlanFromExplain(explain);
    assert(isEofPlan(db, winningPlan));

    explain = coll.find({$nor: [{$alwaysFalse: 1}]}).explain();
    winningPlan = getWinningPlanFromExplain(explain);
    assert(!isEofPlan(db, winningPlan));
    assert(!winningPlan.filter || bsonWoCompare(winningPlan.filter, {}) == 0);
});

/**
 * Verify that {$not: some-expression-that-is-always-false-or-true} is optimized
 */
const coll = db[collName];

// This query is interesting because $addFields and the structure of the query
// require unparsing and parsing it back, and since {$in: []} is optimized to
// $alwaysFalse, this requires parsing {$not: $alwaysFalse}, which fails. If
// {$not: $alwaysFalse} is rewritten to $alwaysTrue, there is no parsing failure
// and the query is simpler.
const q1 = [{$addFields: {t: 0}}, {$match: {$or: [{b: 13, t: 42}], t: {$elemMatch: {$not: {$in: []}}}}}];
let explainResult = coll.explain().aggregate(q1);
let winningPlan = getWinningPlanFromExplain(explainResult);
assert.docEq(winningPlan.filter, {b: {$eq: 13}}, "Winning plan filter should match expected filter");
const matchStage = getAggPlanStage(explainResult, "$match");
const expectedMatchCondition = {
    $and: [{t: {$elemMatch: {}}}, {t: {$eq: 42}}],
};
assert.docEq(matchStage.$match, expectedMatchCondition, "$match stage should have the expected structure");

// Simple cases
const q2 = [{$match: {t: {$elemMatch: {$not: {$in: []}}}}}];
explainResult = coll.explain().aggregate(q2);
winningPlan = getWinningPlanFromExplain(explainResult);
assert.docEq(winningPlan.filter, {t: {"$elemMatch": {}}}, "Winning plan filter should match expected filter");

const q3 = [{$match: {t: {$not: {$in: []}}}}];
explainResult = coll.explain().aggregate(q3);
winningPlan = getWinningPlanFromExplain(explainResult);
assert.eq(winningPlan.hasOwnProperty("filter"), false, "Winning plan should not have a filter property");
