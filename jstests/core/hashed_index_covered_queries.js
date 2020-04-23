/**
 * Test to verify that hashed indexes can cover projections when appropriate. The queries can be
 * covered when neither the query predicate nor projection uses a hashed field.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq().
load("jstests/libs/analyze_plan.js");         // For assertStagesForExplainOfCommand().

const coll = db.compound_hashed_index;
coll.drop();

for (let i = 0; i < 100; i++) {
    assert.commandWorked(coll.insert({a: i, b: (i % 13), c: NumberInt(i % 10)}));
}

/**
 * Runs find command with the 'filter' and 'projection' provided in the input, then validates
 * that the output returned matches 'expectedOutput'. Also runs explain() command on the same find
 * command and validates that all the 'expectedStages' are present in the plan returned.
 */
function validateFindCmdOutputAndPlan({filter, projection, expectedOutput, expectedStages}) {
    const cmdObj = {find: coll.getName(), filter: filter, projection: projection};
    if (expectedOutput) {
        const res = assert.commandWorked(coll.runCommand(cmdObj));
        const ouputArray = new DBCommandCursor(coll.getDB(), res).toArray();

        // We ignore the order since hashed index order is not predictable.
        assert(arrayEq(expectedOutput, ouputArray), ouputArray);
    }

    return assertStagesForExplainOfCommand(
        {coll: coll, cmdObj: cmdObj, expectedStages: expectedStages});
}

/**
 * Runs count command with the 'filter' and 'projection' provided in the input, then validates
 * that the output returned matches 'expectedOutput'. Also runs explain() command on the same count
 * command and validates that all the 'expectedStages' are present in the plan returned.
 */
function validateCountCmdOutputAndPlan({filter, expectedOutput, expectedStages}) {
    const cmdObj = {count: coll.getName(), query: filter};
    const res = assert.commandWorked(coll.runCommand(cmdObj));
    assert.eq(res.n, expectedOutput);
    assertStagesForExplainOfCommand({coll: coll, cmdObj: cmdObj, expectedStages: expectedStages});
}

/**
 * Tests when hashed field is a prefix.
 */
assert.commandWorked(coll.createIndex({b: "hashed", c: -1, a: 1}));

// Verify that queries cannot be covered with hashed field is a prefix.
validateFindCmdOutputAndPlan(
    {filter: {c: 1}, projection: {a: 1, _id: 0}, expectedStages: ['COLLSCAN']});

/**
 * Tests when hashed field is not a prefix.
 */
assert.commandWorked(coll.createIndex({a: 1, b: "hashed", c: -1}));

// Verify that query doesn't get covered when projecting a hashed field.
validateFindCmdOutputAndPlan({
    filter: {a: 26},
    projection: {b: 1, _id: 0},
    expectedOutput: [{b: 0}],
    expectedStages: ['FETCH', 'IXSCAN']
});

// Verify that query doesn't get covered when query is on a hashed field. This is to avoid the
// possibility of hash collision. If two different fields produce the same hash value, there is no
// way to distinguish them without fetching the document.
validateFindCmdOutputAndPlan({
    filter: {a: 26, b: 0},
    projection: {c: 1, _id: 0},
    expectedOutput: [{c: 6}],
    expectedStages: ['FETCH', 'IXSCAN']
});

// Verify that query gets covered when neither query nor project use hashed field.
validateFindCmdOutputAndPlan({
    filter: {a: {$gt: 24, $lt: 27}},
    projection: {c: 1, _id: 0},
    expectedOutput: [{c: 5}, {c: 6}],
    expectedStages: ['IXSCAN', 'PROJECTION_COVERED']
});

// Verify that an empty query with a coverable projection always uses a COLLSCAN.
validateFindCmdOutputAndPlan(
    {filter: {}, projection: {a: 1, _id: 0}, expectedStages: ['COLLSCAN']});

// Verify that COUNT_SCAN cannot be used when query is on a hashed field.
validateCountCmdOutputAndPlan(
    {filter: {a: 26, b: 0}, expectedStages: ['FETCH', 'IXSCAN'], expectedOutput: 1});

// Verify that a count operation with range query on a non-hashed prefix field can use
// COUNT_SCAN.
validateCountCmdOutputAndPlan(
    {filter: {a: {$gt: 25, $lt: 29}}, expectedStages: ["COUNT_SCAN"], expectedOutput: 3});
})();