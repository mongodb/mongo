// Tests optimization rule for pushing match stages over group stages
// @tags: [
//   do_not_wrap_aggregations_in_facets,
//   requires_pipeline_optimization,
//   requires_fcv_71,
// ]

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For 'aggPlanHasStage' and other explain helpers.

const coll = db.grouped_match_push_down;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, x: 10}));
assert.commandWorked(coll.insert({_id: 2, x: 20}));
assert.commandWorked(coll.insert({_id: 3, x: 30}));
assert.commandWorked(coll.insert({_id: 20, d: 2}));

// Asserts end-to-end the optimization of group-project-match stage sequence which includes a rename
// over a dotted path. It evaluates the correctness of the result, as well as whether the
// optimization pushed the predicate down before the aggregation.
function assertOptimizeMatchRenameAggregationPipelineWithDottedRename(
    {pipeline, stageSequence, expectedResult}) {
    const explainOutput = coll.explain().aggregate(pipeline);

    assert(getStageSequence(explainOutput), stageSequence);

    const actualResult = coll.aggregate(pipeline).toArray();
    assert.sameMembers(expectedResult, actualResult);
}

// Assert that a sequence of stages group, project, match over a rename on a dotted path (depth 3)
// will push the predicate before the group stage.
assertOptimizeMatchRenameAggregationPipelineWithDottedRename({
    pipeline: [
        {$group: {_id: {c: '$d'}, c: {$sum: {$const: 1}}}},
        {$project: {m: '$_id.c'}},
        {$match: {m: {$eq: 2}}}
    ],
    stageSequence: ["$cursor", "$project"],
    expectedResult: [{"_id": {"c": 2}, "m": 2}]
});

// Assert that the optimization over group, project, match over a renamed dotted path will push
// down the predicate while the dotted notation is kept to 2 levels.
assertOptimizeMatchRenameAggregationPipelineWithDottedRename({
    pipeline: [
        {$group: {_id: {c: '$d'}, c: {$sum: {$const: 1}}}},
        {$project: {m: '$_id.c'}},
        {$project: {m2: '$m'}},
        {$match: {m2: {$eq: 2}}}
    ],
    stageSequence: ["$cursor", "$project", "project"],
    expectedResult: [{"_id": {"c": 2}, "m2": 2}]
});

// Assert that the optimization over group, project, match over a renamed dotted path will not
// push down the predicate when the rename stage renames a dotted path with depth more than 3.
assertOptimizeMatchRenameAggregationPipelineWithDottedRename({
    pipeline: [
        {$group: {_id: {c: {d: '$d'}}, c: {$sum: {$const: 1}}}},
        {$project: {m: '$_id.c.d'}},
        {$match: {m: {$eq: 2}}}
    ],
    stageSequence: ["$cursor", "$project", "match"],
    expectedResult: [{"_id": {"c": {"d": 2}}, "m": 2}]
});
}());
