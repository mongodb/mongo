(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_count_optimize;
t.drop();

for (let i = 0; i < 100; i++) {
    assert.commandWorked(t.insert({a: i + 10}));
}

const res = runWithParams(
    [
        {key: "internalCascadesOptimizerExplainVersion", value: "v2"},
        {key: "internalCascadesOptimizerUseDescriptiveVarNames", value: true}
    ],
    () => t.explain("executionStats").aggregate([
        {$match: {a: 2}},
        {$addFields: {a1: {$add: ["$x", "$y"]}}},
        {$project: {a: 1, b: 1}},
        {$count: "c"}
    ]));

// Demonstrate the addition in $addFields, along with the projection of "a" and "b", are removed.
const expectedStr =
    `Root [{combinedProjection_2}]
Evaluation [{combinedProjection_2}]
|   EvalPath []
|   |   Const [{}]
|   PathComposeM []
|   |   PathComposeM []
|   |   |   PathKeep [c]
|   |   PathObj []
|   PathComposeM []
|   |   PathField [c]
|   |   PathConstant []
|   |   Variable [field_agg_0]
|   PathField [_id]
|   PathConstant []
|   Variable [groupByProj_0]
GroupBy [{groupByProj_0}]
|   aggregations: 
|       [field_agg_0]
|           FunctionCall [$sum]
|           Const [1]
Evaluation [{groupByProj_0} = Const [null]]
Filter []
|   EvalFilter []
|   |   Variable [evalTemp_0]
|   PathTraverse [1]
|   PathCompare [Eq]
|   Const [2]
PhysicalScan [{'a': evalTemp_0}, cqf_count_optimize_]
`;
assert.eq(expectedStr, removeUUIDsFromExplain(db, res));
}());
