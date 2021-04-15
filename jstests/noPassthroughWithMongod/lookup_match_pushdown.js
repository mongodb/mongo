/**
 * Tests that the $match stage is pushed before $lookup stage.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
load('jstests/libs/analyze_plan.js');         // For getWinningPlan().

const coll = db.lookup_match_pushdown;
coll.drop();
const other = db.lookup_match_pushdown_other;
other.drop();

assert.commandWorked(
    db.adminCommand({"configureFailPoint": 'disablePipelineOptimization', "mode": 'off'}));

assert.commandWorked(coll.insertMany([{_id: 1, x: 5}, {_id: 2, x: 6}]));
assert.commandWorked(
    other.insertMany([{_id: 2, y: 5, z: 10}, {_id: 3, y: 5, z: 12}, {_id: 4, y: 6, z: 10}]));

// Checks that the order of the pipeline stages matches the expected ordering.
function checkPipelineAndResults(pipeline, expectedPipeline, expectedResults) {
    // Check pipeline is as expected.
    const explain = assert.commandWorked(coll.explain().aggregate(pipeline));
    if (expectedPipeline.length > 0) {
        assert.eq(getWinningPlan(explain.stages[0].$cursor.queryPlanner).stage,
                  expectedPipeline[0],
                  explain);
    }
    assert.eq(explain.stages.length, expectedPipeline.length, explain);
    for (let i = 1; i < expectedPipeline.length; i++) {
        assert.eq(Object.keys(explain.stages[i]), expectedPipeline[i], explain);
    }

    // Check results are as expected.
    const res = coll.aggregate(pipeline).toArray();
    assertArrayEq({actual: res, expected: expectedResults});
}

const expectedPipeline = ["COLLSCAN", "$lookup"];

// For $eq and $expr:$eq, we should see the same results (in this particular case).
const expectedResultsEq = [{_id: 1, x: 5, a: {_id: 2, y: 5, z: 10}}];

// Ensure $match gets pushed down into $lookup when $eq is used.
const pipelineEq = [
    {$lookup: {as: "a", from: other.getName(), localField: "x", foreignField: "y"}},
    {$unwind: "$a"},
    {$match: {"a.z": 10, x: {$eq: 5}}}
];
checkPipelineAndResults(pipelineEq, expectedPipeline, expectedResultsEq);

// Ensure $match gets pushed down into $lookup when $expr:$eq is used.
const pipelineExprEq = [
    {$lookup: {as: "a", from: other.getName(), localField: "x", foreignField: "y"}},
    {$unwind: "$a"},
    {$match: {"a.z": 10, $expr: {$eq: ["$x", 5]}}}
];
checkPipelineAndResults(pipelineExprEq, expectedPipeline, expectedResultsEq);

// For $eq and $expr:$gt, we should see the same results (in this particular case).
const expectedResultsGt = [{_id: 2, x: 6, a: {_id: 4, y: 6, z: 10}}];

// Ensure $match gets pushed down into $lookup when $eq is used.
const pipelineGt = [
    {$lookup: {as: "a", from: other.getName(), localField: "x", foreignField: "y"}},
    {$unwind: "$a"},
    {$match: {"a.z": 10, x: {$gt: 5}}}
];
checkPipelineAndResults(pipelineGt, expectedPipeline, expectedResultsGt);

// Ensure $match gets pushed down into $lookup when $expr:$eq is used.
const pipelineExprGt = [
    {$lookup: {as: "a", from: other.getName(), localField: "x", foreignField: "y"}},
    {$unwind: "$a"},
    {$match: {"a.z": 10, $expr: {$gt: ["$x", 5]}}}
];
checkPipelineAndResults(pipelineExprGt, expectedPipeline, expectedResultsGt);
}());
