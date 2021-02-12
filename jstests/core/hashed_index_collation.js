/**
 * Tests to verify that hashed indexes obey collation rules.
 *
 * The tags below are necessary because collation requires that we use read/write commands rather
 * than legacy operations.
 * @tags: [
 *   assumes_unsharded_collection,
 *   requires_find_command,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq().
load("jstests/libs/analyze_plan.js");         // For assertStagesForExplainOfCommand().

const coll = db.hashed_index_collation;
coll.drop();
const collation = {
    locale: "en_US",
    strength: 1
};

/**
 * Runs find command with the 'filter' and 'projection' provided in the input, then validates
 * that the output returned matches 'expectedOutput'. Also runs explain() command on the same find
 * command, validates that all the 'expectedStages' are present in the plan returned and all the
 * 'stagesNotExpected' are not present in the plan.
 */
function validateFindCmdOutputAndPlan(
    {filter, projection = {}, expectedOutput, expectedStages, stagesNotExpected}) {
    const cmdObj =
        {find: coll.getName(), filter: filter, projection: projection, collation: collation};
    const res = assert.commandWorked(coll.runCommand(cmdObj));
    const ouputArray = new DBCommandCursor(coll.getDB(), res).toArray();

    // We ignore the order since hashed index order is not predictable.
    assert(arrayEq(expectedOutput, ouputArray), ouputArray);

    assertStagesForExplainOfCommand({
        coll: coll,
        cmdObj: cmdObj,
        expectedStages: expectedStages,
        stagesNotExpected: stagesNotExpected
    });
}

// Verify that index creation works for compound hashed index with collation, when hashed field is a
// prefix.
assert.commandWorked(
    coll.createIndex({"a.b": "hashed", "a.c": 1, "a.e": -1}, {collation: collation}));

// Insert a series of documents whose fieldnames and values differ only by case.
assert.commandWorked(coll.insert({_id: 0, a: {b: "string", c: "STRING", e: 5}}));
assert.commandWorked(coll.insert({_id: 1, a: {b: "STRING", c: "string", e: 5}}));
assert.commandWorked(coll.insert({_id: 2, A: {B: "string", C: "STRING", E: 5}}));
assert.commandWorked(coll.insert({_id: 3, A: {B: "StrinG", C: "sTRINg", E: 5}}));

// Verify that hashed field can use index in the presence of collation. Also verify that only
// the document's values, not the field names, adhere to the case-insensitive collation.
validateFindCmdOutputAndPlan({
    filter: {"a.b": "string", "a.c": "string"},
    expectedStages: ["IXSCAN", "FETCH"],
    expectedOutput: [
        {_id: 0, a: {b: "string", c: "STRING", e: 5}},
        {_id: 1, a: {b: "STRING", c: "string", e: 5}}
    ]
});

// Verify that the field names doesn't adhere to the case-insensitive collation and uses collection
// scan if the case doesn't match.
validateFindCmdOutputAndPlan({
    filter: {"A.B": "string"},
    expectedStages: ["COLLSCAN"],
    expectedOutput: [
        {_id: 2, A: {B: "string", C: "STRING", E: 5}},
        {_id: 3, A: {B: "StrinG", C: "sTRINg", E: 5}}
    ]
});

// Verify that $or query with collation returns correct results.
validateFindCmdOutputAndPlan({
    filter: {$or: [{"a.b": "string_1"}, {"a.b": "string", "a.c": "string"}]},
    expectedStages: ["OR"],
    stagesNotExpected: ["COLLSCAN"],  // Verify that both the OR stages use index scan.
    expectedOutput: [
        {_id: 0, a: {b: "string", c: "STRING", e: 5}},
        {_id: 1, a: {b: "STRING", c: "string", e: 5}}
    ]
});

/**
 * When hashed field is not a prefix.
 */
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(
    coll.createIndex({"a.b": 1, "a.c": "hashed", "a.e": -1}, {collation: collation}));

// Hashed indexes with collation can be covered, if the query predicate restrict strings from being
// returned.
validateFindCmdOutputAndPlan({
    filter: {"a.b": "string", "a.e": {$type: "number"}},
    projection: {"a.e": 1, _id: 0},
    expectedStages: ["IXSCAN"],
    stagesNotExpected: ["FETCH"],
    expectedOutput: [{a: {e: 5}}, {a: {e: 5}}]
});

// Hashed indexes with collation cannot be covered, if the query predicate doesn't restrict strings
// from being returneds.
validateFindCmdOutputAndPlan({
    filter: {"a.b": "string"},
    projection: {"a.e": 1, _id: 0},
    expectedStages: ["IXSCAN", "FETCH"],
    expectedOutput: [{a: {e: 5}}, {a: {e: 5}}]
});
})();
