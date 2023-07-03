// Tests optimization rule for pushing match stages over group stages
// @tags: [
//   do_not_wrap_aggregations_in_facets,
//   requires_pipeline_optimization,
// ]

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For 'aggPlanHasStage' and other explain helpers.

const coll = db.grouped_match_push_down;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, x: 10}));
assert.commandWorked(coll.insert({_id: 2, x: 20}));
assert.commandWorked(coll.insert({_id: 3, x: 30}));

// Asserts end-to-end the optimization of group-project-match stage sequence which includes a rename
// over a dotted path. It evaluates the correctness of the result, as well as whether the
// optimization pushed the predicate down before the aggregation.
function assertOptimizeMatchRenameAggregationPipelineWithDottedRename(
    {pipeline, expectedStages, expectedResult} = {}) {
    const explainOutput = coll.explain().aggregate(pipeline);

    if (expectedStages) {
        for (let expectedStage of expectedStages) {
            assert(aggPlanHasStage(explainOutput, expectedStage), explainOutput);
        }
    }

    if (expectedResult) {
        const actualResult = coll.aggregate(pipeline).toArray();
        assert.sameMembers(expectedResult, actualResult);
    }
}

assert.commandWorked(coll.insert({_id: 20, d: 2}));
// Assert that a sequence of stages group, project, match over a rename on a dotted path (depth 3)
// will push the predicate before the group stage.
assertOptimizeMatchRenameAggregationPipelineWithDottedRename({
    pipeline: [
        {$group: {_id: {c: '$d'}, c: {$sum: {$const: 1}}}},
        {$project: {m: '$_id.c'}},
        {$match: {m: {$eq: 2}}}
    ],
    expectedStages: ["GROUP"],
    expectedResult: [{"_id": {"c": 2}, "m": 2}]
});

// Assert that the optimization over group, project, match over a renamed dotted path will not
// push down the predicate over multiple projection stages.
assertOptimizeMatchRenameAggregationPipelineWithDottedRename({
    pipeline: [
        {$group: {_id: {c: '$d'}, c: {$sum: {$const: 1}}}},
        {$project: {m: '$_id.c'}},
        {$project: {m2: '$m'}},
        {$match: {m2: {$eq: 2}}}
    ],
    // the optimization will not push the filter, thus, the query will perform a COLLSCAN. The
    // system does not have an index to use.
    expectedStages: ["COLLSCAN"],
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
    expectedStages: ["COLLSCAN"],
    expectedResult: [{"_id": {"c": {"d": 2}}, "m": 2}]
});
}());
