/**
 * Tests to verify that the queries return correct results in the presence of partial hashed
 * index and sparse index.
 * @tags: [
 *   requires_fcv_42,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq().
load("jstests/libs/analyze_plan.js");         // For assertStagesForExplainOfCommand().

const coll = db.hashed_partial_index;
coll.drop();
assert.commandWorked(coll.insert({}));
assert.commandWorked(coll.insert({a: null}));
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({b: 4}));
assert.commandWorked(coll.insert({a: 1, b: 6}));

/**
 * Runs explain() operation on 'cmdObj' and verifies that all the stages in 'expectedStages' are
 * present exactly once in the plan returned. When 'stagesNotExpected' array is passed, also
 * verifies that none of those stages are present in the explain() plan.
 */
function assertStagesForExplainOfCommand({coll, cmdObj, expectedStages, stagesNotExpected}) {
    const plan = assert.commandWorked(coll.runCommand({explain: cmdObj}));
    const winningPlan = plan.queryPlanner.winningPlan;
    for (let expectedStage of expectedStages) {
        assert(planHasStage(coll.getDB(), winningPlan, expectedStage),
               "Could not find stage " + expectedStage + ". Plan: " + tojson(plan));
    }
    for (let stage of (stagesNotExpected || [])) {
        assert(!planHasStage(coll.getDB(), winningPlan, stage),
               "Found stage " + stage + " when not expected. Plan: " + tojson(plan));
    }
    return plan;
}

/**
 * Runs find command with the 'filter' and validates that the output returned matches
 * 'expectedOutput'. Also runs explain() command on the same find command and validates that all
 * the 'expectedStages' are present in the plan returned.
 */
function validateFindCmdOutputAndPlan({filter, expectedStages, expectedOutput}) {
    const cmdObj = {find: coll.getName(), filter: filter, projection: {_id: 0}};
    if (expectedOutput) {
        const res = assert.commandWorked(coll.runCommand(cmdObj));
        const ouputArray = new DBCommandCursor(coll.getDB(), res).toArray();

        // We ignore the order since hashed index order is not predictable.
        assert(arrayEq(expectedOutput, ouputArray), ouputArray);
    }
    assertStagesForExplainOfCommand({coll: coll, cmdObj: cmdObj, expectedStages: expectedStages});
}

function testSparseHashedIndex(indexSpec) {
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndex(indexSpec, {sparse: true}));

    // Verify index not used for null/missing queries with sparse index.
    validateFindCmdOutputAndPlan({filter: {a: null}, expectedStages: ["COLLSCAN"]});
    validateFindCmdOutputAndPlan({filter: {a: {$exists: false}}, expectedStages: ["COLLSCAN"]});

    // Test {$exists: false} when hashed field is not a prefix and index is sparse.
    validateFindCmdOutputAndPlan({
        filter: {a: {$exists: false}},
        expectedOutput: [{b: 4}, {}],
        expectedStages: ["COLLSCAN"],
        stagesNotExpected: ["IXSCAN"]
    });
}

/**
 * Test sparse indexes with hashed prefix.
 */
testSparseHashedIndex({a: "hashed"});

/**
 * Tests for partial indexes.
 */
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({b: "hashed"}, {partialFilterExpression: {b: {$gt: 5}}}));

// Verify that index is not used if the query predicate doesn't match the
// 'partialFilterExpression'.
validateFindCmdOutputAndPlan({filter: {b: 4}, expectedStages: ["COLLSCAN"]});

// Verify that index is used if the query predicate matches the 'partialFilterExpression'.
validateFindCmdOutputAndPlan(
    {filter: {b: 6}, expectedOutput: [{a: 1, b: 6}], expectedStages: ["IXSCAN", "FETCH"]});
})();
