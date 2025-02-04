/**
 * Test that queries eligible for the $expr + $in rewrite to MatchExpression return the same results
 * as when the 'disablePipelineOptimization' and 'disableMatchExpressionOptimization' failpoints are
 * enabled.
 *
 * @tags: [
 *   requires_fcv_81
 * ]
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

const coll = db[jsTestName()];
coll.drop();

const docs = [
    {_id: 0, category: "clothing"},
    {_id: 1, category: "electronics"},
    {_id: 2, category: "materials"},
    {_id: 3},
    {_id: 4, category: null},
    {_id: 5, category: {}},
    {_id: 6, category: []},
    {_id: 7, category: [[]]},
    {_id: 8, category: [null]},
    {_id: 9, category: [[], [""]]},
    {_id: 10, category: ["clothing", "electronics"]},
    {_id: 11, category: ["materials", "clothing", "electronics"]},
    {_id: 12, category: [["clothing"], ["electronics"]]},
    {_id: 13, category: /clothing/},
    {_id: 14, category: /electronics/},
    {_id: 15, category: [{}]},
    {_id: 16, category: [["clothing", "electronics"]]},
    {_id: 17, category: [[["clothing", "materials", "electronics"]]]},
    {_id: 18, category: [[["clothing", "electronics"]]]},
    {_id: 19, category: [[[null]]]},
    {_id: 20, category: "clothings"},
    {_id: 21, category: 1},
    {_id: 22, category: 1.0},
    {_id: 23, category: NumberDecimal("1.00000000000000")},
    {_id: 24, category: NumberLong(1)},
    {_id: 25, category: {$toDouble: 1}},
    {_id: 26, "category.a": "clothing"},
    {_id: 27, category: {a: "clothing"}},
    {_id: 28, category: {b: "clothing"}},
    {_id: 29, category: [{a: "clothing"}, {a: "electronics"}, {}]}
];
assert.commandWorked(coll.insertMany(docs));

// Create indexes on "category" and "category.a"
assert.commandWorked(coll.createIndexes([{category: 1}, {"category.a": 1}]));

const testCases = [
    {$in: ["$category", ["clothing", "materials"]]},
    {$in: ["$category", [1]]},
    {$in: ["$category.a", ["clothing", "electronics"]]},
    {$in: ["$category", [{}]]},
    {$in: ["$category", [{a: 'clothing'}]]},
    {$in: ["$category", [{$toDouble: 1}]]},
    {$in: ["$category", [{$literal: {$toDouble: 1}}]]}
];

let optimizedFindResults = [];
let optimizedAggResults = [];

testCases.forEach((expr) => {
    optimizedFindResults.push(coll.find({$expr: expr}).toArray());
    optimizedAggResults.push(coll.aggregate([{$match: {$expr: expr}}]).toArray());
});

// Verify that optimized results are equal to results when optimizations are disabled.
assert.commandWorked(
    db.adminCommand({configureFailPoint: "disableMatchExpressionOptimization", mode: "alwaysOn"}));
assert.commandWorked(
    db.adminCommand({configureFailPoint: "disablePipelineOptimization", mode: "alwaysOn"}));

testCases.forEach((expr, i) => {
    const pipeline = [{$_internalInhibitOptimization: {}}, {$match: {$expr: expr}}];
    assert.sameMembers(optimizedFindResults[i], coll.find({$expr: expr}).toArray());
    assert.sameMembers(optimizedAggResults[i], coll.aggregate(pipeline).toArray());
});

MongoRunner.stopMongod(conn);
