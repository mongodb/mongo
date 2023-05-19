/**
 * Test to verify the behaviour of find, count, distinct operations in the presence of compound
 * hashed indexes.
 * @tags: [
 *   assumes_read_concern_local,
 * ]
 */
(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");  // For arrayEq().
load("jstests/libs/analyze_plan.js");         // For assertStagesForExplainOfCommand().

const collNamePrefix = 'hashed_index_queries_';
let collCount = 0;
let coll;

let docs = [];
let docId = 0;
for (let i = 0; i < 100; i++) {
    docs.push({_id: docId++, a: i, b: {subObj: "str_" + (i % 13)}, c: NumberInt(i % 10)});
    docs.push({_id: docId++, a: i, b: (i % 13), c: NumberInt(i % 10)});
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

/**
 * Runs count command with the 'filter' and validates that the output returned matches
 * 'expectedOutput'. Also runs explain() command on the same count command and validates that all
 * the 'expectedStages' are present in the plan returned.
 */
function validateCountCmdOutputAndPlan({filter, expectedStages, expectedOutput}) {
    const cmdObj = {count: coll.getName(), query: filter};
    if (expectedOutput) {
        const res = assert.commandWorked(coll.runCommand(cmdObj));
        assert.eq(res.n, expectedOutput);
    }
    assertStagesForExplainOfCommand({coll: coll, cmdObj: cmdObj, expectedStages: expectedStages});
}
/**
 * Tests for 'find' operation when hashed field is prefix.
 */
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({b: "hashed", c: -1}));
assert.commandWorked(coll.insert(docs));

// Verify that index is not used for a range query on a hashed field.
validateFindCmdOutputAndPlan({filter: {b: {$gt: 10, $lt: 12}}, expectedStages: ["COLLSCAN"]});

// Verify that index is not used for a query on a hashed field's sub-object.
validateFindCmdOutputAndPlan({filter: {"b.subObj": "str_10"}, expectedStages: ["COLLSCAN"]});

// Verify that index is used for a query on a hashed field.
validateFindCmdOutputAndPlan({
    filter: {b: {subObj: "str_11"}},
    expectedOutput: [
        {a: 11, b: {subObj: "str_11"}, c: 1},
        {a: 24, b: {subObj: "str_11"}, c: 4},
        {a: 37, b: {subObj: "str_11"}, c: 7},
        {a: 50, b: {subObj: "str_11"}, c: 0},
        {a: 63, b: {subObj: "str_11"}, c: 3},
        {a: 76, b: {subObj: "str_11"}, c: 6},
        {a: 89, b: {subObj: "str_11"}, c: 9},
    ],
    expectedStages: ["IXSCAN", "FETCH"],
});

/**
 * Tests for 'find' operation when hashed field is not a prefix.
 */
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({a: 1, b: "hashed", c: -1}));
assert.commandWorked(coll.insert(docs));

// Verify $in query can use point interval bounds on hashed fields and non-hashed fields.
validateFindCmdOutputAndPlan({
    filter:
        {a: {$in: [38, 37]}, b: {$in: [{subObj: "str_12"}, {subObj: "str_11"}]}, c: {$in: [7, 8]}},
    expectedOutput: [{a: 37, b: {subObj: "str_11"}, c: 7}, {a: 38, b: {subObj: "str_12"}, c: 8}],
    expectedStages: ["IXSCAN", "FETCH"]
});

// Verify that a range query on a non-hashed prefix field can use index.
validateFindCmdOutputAndPlan({
    filter: {a: {$gt: 25, $lt: 29}, b: 0},
    expectedOutput: [{a: 26, b: 0, c: 6}],
    expectedStages: ["IXSCAN", "FETCH"]
});

/**
 * Tests for 'count' operation when hashed field is prefix.
 */
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({b: "hashed", a: 1}));
assert.commandWorked(coll.insert(docs));

// Verify that index is not used for a range query on a hashed field.
validateCountCmdOutputAndPlan(
    {filter: {b: {$gt: 10, $lt: 12}}, expectedOutput: 7, expectedStages: ["COLLSCAN"]});

// Verify that index is used for a query on a hashed field.
validateCountCmdOutputAndPlan(
    {filter: {b: {subObj: "str_10"}}, expectedOutput: 7, expectedStages: ["IXSCAN", "FETCH"]});

/**
 * Tests for 'count' operation when hashed field is not a prefix.
 */
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({a: 1, b: "hashed", c: -1}));
assert.commandWorked(coll.insert(docs));

// Verify that range query on a non-hashed prefix field can use index.
validateCountCmdOutputAndPlan({
    filter: {a: {$gt: 25, $lt: 29}, b: 0},
    expectedOutput: 1,
    expectedStages: ["IXSCAN", "FETCH"]
});
})();
