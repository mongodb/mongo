/**
 * Tests to verify that the queries return correct results in the presence of partial hashed
 * index and sparse index. The test verifies compound hashed index with hashed prefix and non-hashed
 * prefix.
 * @tags: [
 *   sbe_incompatible,
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

/**
 * Tests for sparse indexes.
 */
assert.commandWorked(coll.createIndex({a: 1, b: "hashed", c: -1}, {sparse: true}));

// Verify index not used for null/missing queries with sparse index.
validateFindCmdOutputAndPlan({filter: {a: null}, expectedStages: ["COLLSCAN"]});
validateFindCmdOutputAndPlan({filter: {a: {$exists: false}}, expectedStages: ["COLLSCAN"]});

// Verify index can be used for non-null queries with sparse index.
validateFindCmdOutputAndPlan({
    filter: {a: {$exists: true}},
    expectedOutput: [{a: null}, {a: 1}, {a: 1, b: 6}],
    expectedStages: ["IXSCAN", "FETCH"]
});

validateFindCmdOutputAndPlan(
    {filter: {a: 1, b: 6}, expectedOutput: [{a: 1, b: 6}], expectedStages: ["IXSCAN", "FETCH"]});

/**
 * Tests for partial indexes.
 */
[{b: "hashed", c: 1}, {b: 1, c: "hashed", d: 1}].forEach((index) => {
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndex(index, {partialFilterExpression: {b: {$gt: 5}}}));

    // Verify that index is not used if the query predicate doesn't match the
    // 'partialFilterExpression'.
    validateFindCmdOutputAndPlan({filter: {b: 4}, expectedStages: ["COLLSCAN"]});

    // Verify that index is used if the query predicate matches the 'partialFilterExpression'.
    validateFindCmdOutputAndPlan(
        {filter: {b: 6}, expectedOutput: [{a: 1, b: 6}], expectedStages: ["IXSCAN", "FETCH"]});
});
})();