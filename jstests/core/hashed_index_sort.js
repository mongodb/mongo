/**
 * Test to verify the behaviour of compound hashed indexes when 'sort' operation is used along with
 * the 'find' command. The test verifies compound hashed index with hashed prefix and non-hashed
 * prefix.
 * @tags: [assumes_unsharded_collection, requires_fcv_44]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For assertStagesForExplainOfCommand().

const coll = db.hashed_index_sort;
coll.drop();

for (let i = 0; i < 5; i++) {
    for (let j = 0; j < 5; j++) {
        assert.commandWorked(coll.insert({_id: (10 * i) + j, a: i, b: j, c: i + j, d: i - j}));
    }
}

/**
 * Runs find command with the 'filter', 'sort' and 'project' provided in the input, then validates
 * that the output returned matches 'expectedOutput'. Also runs explain() command on the same find
 * command, validates that all the 'expectedStages' are present in the plan returned and all the
 * 'stagesNotExpected' are not present in the plan.
 */
function validateFindCmdOutputAndPlan(
    {filter, project: project, sort: sort, expectedStages, stagesNotExpected, expectedOutput}) {
    const cmdObj = {
        find: coll.getName(),
        filter: filter,
        sort: sort,
        projection: project ||
            {
                _id: 0
            }
    };
    if (expectedOutput) {
        const res = assert.commandWorked(coll.runCommand(cmdObj));
        const ouputArray = new DBCommandCursor(coll.getDB(), res).toArray();

        // Make sure that the documents returned are in the same order as 'expectedOutput'.
        assert.eq(expectedOutput, ouputArray, ouputArray);
    }
    assertStagesForExplainOfCommand({
        coll: coll,
        cmdObj: cmdObj,
        expectedStages: expectedStages,
        stagesNotExpected: stagesNotExpected
    });
}

/**
 * Tests when hashed field is prefix.
 */
assert.commandWorked(coll.createIndex({a: "hashed", b: -1, c: 1}));

// Verify that an exact match predicate on hashed field (prefix) and sort with an immediate range
// field can use 'SORT_MERGE'.
validateFindCmdOutputAndPlan({
    filter: {a: 1},
    project: {b: 1, c: 1, _id: 0},
    sort: {b: 1},
    expectedOutput: [
        {b: 0, c: 1},
        {b: 1, c: 2},
        {b: 2, c: 3},
        {b: 3, c: 4},
        {b: 4, c: 5},
    ],
    expectedStages: ["IXSCAN", "FETCH", "SORT_MERGE"]
});

// Verify that a list of exact match predicates on hashed field (prefix) and sort with an immediate
// range field can use 'SORT_MERGE'.
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [1, 2]}},
    project: {b: 1, _id: 0},
    sort: {b: 1},
    expectedOutput:
        [{b: 0}, {b: 0}, {b: 1}, {b: 1}, {b: 2}, {b: 2}, {b: 3}, {b: 3}, {b: 4}, {b: 4}],
    expectedStages: ["FETCH", "SORT_MERGE"],
    stagesNotExpected: ["COLLSCAN"]
});

// Sort on index fields which do not immediately follow the hashed field cannot use SORT_MERGE.
validateFindCmdOutputAndPlan({
    coll: coll,
    filter: {a: 1},
    sort: {c: 1},
    expectedOutput: [
        {a: 1, b: 0, c: 1, d: 1},
        {a: 1, b: 1, c: 2, d: 0},
        {a: 1, b: 2, c: 3, d: -1},
        {a: 1, b: 3, c: 4, d: -2},
        {a: 1, b: 4, c: 5, d: -3},
    ],
    expectedStages: ["IXSCAN", "SORT"]
});

/**
 * Tests when hashed field is not a prefix.
 */
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({a: 1, b: -1, c: "hashed", d: 1}));

// Verify that an exact match predicate on range field (prefix) and sort with an immediate range
// field doesn't require any additional sort stages. The entire operation can be answered by the
// index.
validateFindCmdOutputAndPlan({
    filter: {a: 1},
    project: {_id: 0, a: 1, b: 1},
    sort: {b: 1},
    expectedOutput: [
        {a: 1, b: 0},
        {a: 1, b: 1},
        {a: 1, b: 2},
        {a: 1, b: 3},
        {a: 1, b: 4},
    ],
    expectedStages: ["IXSCAN"],
    stagesNotExpected: ["SORT_MERGE", "SORT", "FETCH"]
});

// Verify that the sort can use index when there is no filter and the sort order is a non-hashed
// prefix of the index pattern.
validateFindCmdOutputAndPlan({
    filter: {},
    project: {_id: 0, a: 1, b: 1},
    sort: {a: 1, b: -1},
    expectedOutput: [
        {a: 0, b: 4}, {a: 0, b: 3}, {a: 0, b: 2}, {a: 0, b: 1}, {a: 0, b: 0},
        {a: 1, b: 4}, {a: 1, b: 3}, {a: 1, b: 2}, {a: 1, b: 1}, {a: 1, b: 0},
        {a: 2, b: 4}, {a: 2, b: 3}, {a: 2, b: 2}, {a: 2, b: 1}, {a: 2, b: 0},
        {a: 3, b: 4}, {a: 3, b: 3}, {a: 3, b: 2}, {a: 3, b: 1}, {a: 3, b: 0},
        {a: 4, b: 4}, {a: 4, b: 3}, {a: 4, b: 2}, {a: 4, b: 1}, {a: 4, b: 0},
    ],
    expectedStages: ["IXSCAN"],
    stagesNotExpected: ["SORT_MERGE", "SORT", "FETCH"]
});

// Verify that the sort cannot use index when there is no filter and the sort order uses a hashed
// field from the index.
validateFindCmdOutputAndPlan({
    filter: {},
    project: {_id: 0, a: 1, b: 1},
    sort: {a: 1, b: -1, c: 1},
    expectedStages: ["SORT", "COLLSCAN"],
});

// Verify that a list of exact match predicates on range field (prefix) and sort with an immediate
// range field can use 'SORT_MERGE'. The entire operation will not require a FETCH.
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [1, 2]}},
    project: {_id: 0, a: 1, b: 1},
    sort: {b: 1},
    nReturned: 20,
    docsExamined: 0,
    expectedStages: ["SORT_MERGE"],
    stagesNotExpected: ["COLLSCAN", "FETCH"]
});

// Verify that query predicate and sort on non-hashed fields can be answered by the index but
// require a sort stage, if the 'sort' field is not immediately after 'query' field in the index.
validateFindCmdOutputAndPlan({
    filter: {a: 1},
    project: {_id: 0, d: 1, b: 1},
    sort: {d: 1},
    expectedOutput: [{b: 4, d: -3}, {b: 3, d: -2}, {b: 2, d: -1}, {b: 1, d: 0}, {b: 0, d: 1}],
    expectedStages: ["IXSCAN", "SORT"],
    stagesNotExpected: ["COLLSCAN", "FETCH"]
});

//  Verify that sort on a hashed field required a FETCH and a SORT stage.
validateFindCmdOutputAndPlan({
    filter: {a: 1, b: 1},
    project: {_id: 0, c: 1},
    sort: {c: 1},
    expectedOutput: [{c: 2}],
    expectedStages: ["IXSCAN", "FETCH", "SORT"]
});
})();