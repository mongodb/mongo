/**
 * Tests for the new tie breaking behaviour with totalDocsExamined.
 * Plans in tests only tie on classic engine, not SBE.
 */

"use strict";

import {getPlanStages} from "jstests/libs/analyze_plan.js";

function testTieBreaking(breakTies, expectedPlanCount, checkAgainstOriginal) {
    const expectedDocsExamined = 1;
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryPlanTieBreakingWithIndexHeuristics: breakTies}));
    const stats = assert.commandWorked(
        coll.find({a: "mouse", b: /not rat/, c: /capybara/, d: /degu/}).explain(true));

    // Check we're generating the expected number of plans.
    assert.eq(stats.executionStats.allPlansExecution.length, expectedPlanCount);

    // Check that all plans tie on their score.
    const scores = stats.executionStats.allPlansExecution.map(a => a.score);
    assert.eq(new Set(scores).size, 1);

    if (checkAgainstOriginal) {
        return getPlanStages(stats, "IXSCAN");
    }

    if (breakTies) {
        assert.eq(stats.executionStats.totalDocsExamined, expectedDocsExamined);
    } else {
        assert.gt(stats.executionStats.totalDocsExamined, expectedDocsExamined);
    }
}

function testTieBreakingScenarios(expectedPlanCount, checkAgainstOriginal) {
    // Test that default behaviour chooses suboptimal plan (docsExamined > 1).
    const statsWithoutTieBreaking = testTieBreaking(false, expectedPlanCount, checkAgainstOriginal);
    // Test that tie-breaking code helps to choose the correct plan (docsExamined = 1).
    const statsWithTieBreaking = testTieBreaking(true, expectedPlanCount, checkAgainstOriginal);

    // If true, test that we choose the same plan with and without the query knob.
    if (checkAgainstOriginal) {
        assert.eq(statsWithoutTieBreaking, statsWithTieBreaking);
    }
}

// Make sure we are testing on the classic engine.
const options = {
    setParameter: {internalQueryFrameworkControl: "forceClassicEngine"}
};
const conn = MongoRunner.runMongod(options);
assert.neq(null, conn, 'mongod was unable to start up with options: ' + tojson(options));
const db = conn.getDB('test');

const coll = db.multiplanner_tie_breaking;
coll.drop();

const docs = [{a: "mouse", b: "not rat", c: "capybara", d: "degu"}];
for (let i = 0; i < 110; i++) {
    docs.push({a: "mouse", b: "rat", c: "capybara", d: "degu"});
}
assert.commandWorked(coll.insertMany(docs));

// Two tied plans that still tie after tie breaking.
// Check that we still choose the same plan as we would without the query knob.
assert.commandWorked(coll.createIndex({a: 1, c: 1}));
assert.commandWorked(coll.createIndex({a: 1, d: 1}));
testTieBreakingScenarios(2, true);
assert.commandWorked(coll.dropIndex({a: 1, d: 1}));

// Two tied plans with one winner.
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
testTieBreakingScenarios(2, false);

// Three tied plans with one winner.
assert.commandWorked(coll.createIndex({c: 1, d: 1}));
testTieBreakingScenarios(3, false);

// Four tied plans with two winners.
// Note that we still have a tie after this but choose arbitrarily.
assert.commandWorked(coll.createIndex({b: 1, c: 1}));
testTieBreakingScenarios(4, false);

MongoRunner.stopMongod(conn);
