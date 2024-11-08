// Tests that the aggregation pipeline correctly coalesces a $project stage at the front of the
// pipeline that can be covered by a normal query.
// @tags: [
//   do_not_wrap_aggregations_in_facets,
//   requires_pipeline_optimization,
// ]
import {
    documentEq,
    orderedArrayEq,
} from "jstests/aggregation/extras/utils.js";
import {
    getPlanStages,
    getWinningPlanFromExplain,
    isAggregationPlan,
    isQueryPlan,
} from "jstests/libs/query/analyze_plan.js";
import {
    checkSbeCompletelyDisabled,
    checkSbeFullyEnabled,
    checkSbeRestrictedOrFullyEnabled
} from "jstests/libs/query/sbe_util.js";

let coll = db.remove_redundant_projects;
coll.drop();

assert.commandWorked(coll.insert({_id: {a: 1, b: 1}, a: 1, c: {d: 1}, e: ['elem1']}));

let indexSpec = {a: 1, 'c.d': 1, 'e.0': 1};

const sbeFullyEnabled = checkSbeFullyEnabled(db);
const sbeRestricted = checkSbeRestrictedOrFullyEnabled(db);

/**
 * Helper to test that for a given pipeline, the same results are returned whether or not an
 * index is present.  Also tests whether a projection is absorbed by the pipeline
 * ('expectProjectToCoalesce') and the corresponding project stage ('removedProjectStage') does
 * not exist in the explain output. When 'expectProjectToCoalesce' is true, the caller should
 * specify which projects are expected to coalesce in 'expectedCoalescedProjects'.
 */
function assertResultsMatch({
    pipeline = [],
    expectProjectToCoalesce = false,
    expectedCoalescedProjects = [],
    removedProjectStage = null,
    index = indexSpec,
    pipelineOptimizedAway = false
} = {}) {
    // Add a match stage to ensure index scans are considered for planning (workaround for
    // SERVER-20066).
    pipeline = [{$match: {a: {$gte: 0}}}].concat(pipeline);

    // Once with an index.
    assert.commandWorked(coll.createIndex(index));
    let explain = coll.explain().aggregate(pipeline);
    let resultsWithIndex = coll.aggregate(pipeline).toArray();

    // Projection does not get pushed down when sharding filter is used.
    if (!explain.hasOwnProperty("shards")) {
        let result;

        if (pipelineOptimizedAway) {
            assert(isQueryPlan(explain), explain);
            result = getWinningPlanFromExplain(explain.queryPlanner);
        } else {
            assert(isAggregationPlan(explain), explain);
            result = getWinningPlanFromExplain(explain.stages[0].$cursor.queryPlanner);
        }

        // Check that $project uses the query system and all expectedCoalescedProjects are
        // actually present in explain.
        if (expectProjectToCoalesce) {
            assert.gte(
                expectedCoalescedProjects.length,
                1,
                "If we expect project to coalesce, there should be at least one such projection in expectedCoalescedProjects " +
                    tojson(expectedCoalescedProjects));

            const projects = [
                ...getPlanStages(result, "PROJECTION_DEFAULT"),
                ...getPlanStages(result, "PROJECTION_COVERED"),
                ...getPlanStages(result, "PROJECTION_SIMPLE")
            ];

            assert.gte(projects.length, 1, explain);

            const areAllexpectedCoalescedProjectsPresent =
                expectedCoalescedProjects.every(coalescedProject => {
                    return projects.some(project => {
                        return documentEq(project.transformBy, coalescedProject);
                    });
                });

            assert(areAllexpectedCoalescedProjectsPresent,
                   "There were missing or extra coalesced projects in " +
                       tojson(expectedCoalescedProjects) + " with explain " + tojson(explain));
        }

        if (!pipelineOptimizedAway) {
            // Check that $project was removed from pipeline and pushed to the query system.
            explain.stages.forEach(function(stage) {
                if (stage.hasOwnProperty("$project"))
                    assert.neq(removedProjectStage, stage["$project"], explain);
            });
        }
    }

    // Again without an index.
    assert.commandWorked(coll.dropIndex(index));
    let resultsWithoutIndex = coll.aggregate(pipeline).toArray();

    assert(orderedArrayEq(resultsWithIndex, resultsWithoutIndex));
}

// Test that covered projections correctly use the query system for projection and the $project
// stage is removed from the pipeline.
assertResultsMatch({
    pipeline: [{$project: {_id: 0, a: 1}}],
    expectProjectToCoalesce: true,
    expectedCoalescedProjects: [{"a": true, "_id": false}],
    pipelineOptimizedAway: true
});
assertResultsMatch({
    pipeline: [{$project: {_id: 0, a: 1}}, {$group: {_id: null, a: {$sum: "$a"}}}],
    expectProjectToCoalesce: true,
    expectedCoalescedProjects: [{"a": true, "_id": false}],
    removedProjectStage: {_id: 0, a: 1},
    pipelineOptimizedAway: sbeRestricted
});
assertResultsMatch({
    pipeline: [{$sort: {a: -1}}, {$project: {_id: 0, a: 1}}],
    expectProjectToCoalesce: true,
    expectedCoalescedProjects: [{"a": true, "_id": false}],
    pipelineOptimizedAway: true
});
assertResultsMatch({
    pipeline: [
        {$sort: {a: 1, 'c.d': 1}},
        {$project: {_id: 0, a: 1}},
        {$group: {_id: "$a", a: {$sum: "$a"}}}
    ],
    expectProjectToCoalesce: true,
    expectedCoalescedProjects: [{"a": true, "_id": false}],
    removedProjectStage: {_id: 0, a: 1},
    pipelineOptimizedAway: sbeRestricted
});
assertResultsMatch({
    pipeline: [{$project: {_id: 0, c: {d: 1}}}],
    expectProjectToCoalesce: true,
    expectedCoalescedProjects: [{"c": {"d": true}, "_id": false}],
    pipelineOptimizedAway: true
});

// Test that projections with renamed fields are removed from the pipeline.
assertResultsMatch({
    pipeline: [{$project: {_id: 0, f: "$a"}}],
    expectProjectToCoalesce: true,
    expectedCoalescedProjects: [{"f": "$a", "_id": false}],
    pipelineOptimizedAway: true
});
assertResultsMatch({
    pipeline: [{$project: {_id: 0, a: 1, f: "$a"}}],
    expectProjectToCoalesce: true,
    expectedCoalescedProjects: [{"a": true, "f": "$a", "_id": false}],
    pipelineOptimizedAway: true
});

// Test that uncovered projections are removed from the pipeline.
assertResultsMatch({
    pipeline: [{$sort: {a: 1}}, {$project: {_id: 1, b: 1}}],
    expectProjectToCoalesce: true,
    expectedCoalescedProjects: [{"_id": true, "b": true}],
    pipelineOptimizedAway: true
});

// When SBE is not enabled, we end up pushing down a projection which is internally generated by the
// aggregation subsystem's dependency analysis logic.
assertResultsMatch({
    pipeline: [{$sort: {a: 1}}, {$group: {_id: "$_id", a: {$sum: "$a"}}}, {$project: {arr: 1}}],
    expectProjectToCoalesce: checkSbeCompletelyDisabled(db) || sbeFullyEnabled,
    expectedCoalescedProjects: sbeFullyEnabled ? [{"_id": true, "arr": true}]
                                               : [{"_id": 1, "a": 1}],
    pipelineOptimizedAway: sbeFullyEnabled
});

// Test that projections with computed fields are removed from the pipeline.
assertResultsMatch({
    pipeline: [{$project: {computedField: {$sum: "$a"}}}],
    expectProjectToCoalesce: true,
    expectedCoalescedProjects: [{"_id": true, "computedField": {"$sum": ["$a"]}}],
    pipelineOptimizedAway: true
});
assertResultsMatch({
    pipeline: [{$project: {a: ["$a", "$b"]}}],
    expectProjectToCoalesce: true,
    expectedCoalescedProjects: [{"_id": true, "a": ["$a", "$b"]}],
    pipelineOptimizedAway: true
});
assertResultsMatch({
    pipeline:
        [{$project: {e: {$filter: {input: "$e", as: "item", cond: {"$eq": ["$$item", "elem0"]}}}}}],
    expectProjectToCoalesce: true,
    expectedCoalescedProjects: [{
        "_id": true,
        "e": {
            "$filter":
                {"input": "$e", "as": "item", "cond": {"$eq": ["$$item", {"$const": "elem0"}]}}
        }
    }],
    pipelineOptimizedAway: true
});

assertResultsMatch({
    pipeline: [
        {$project: {_id: 0, a: 1}},
        {$group: {_id: "$a", c: {$sum: "$c"}, a: {$sum: "$a"}}},
        {$project: {_id: 0}}
    ],
    expectProjectToCoalesce: true,
    expectedCoalescedProjects: sbeFullyEnabled ? [{"a": true, "_id": false}, {"_id": false}]
                                               : [{"a": true, "_id": false}],
    pipelineOptimizedAway: sbeFullyEnabled
});

// Test that projections on _id with nested fields are removed from pipeline.
indexSpec = {
    '_id.a': 1,
    a: 1
};
assertResultsMatch({
    pipeline: [{$match: {"_id.a": 1}}, {$project: {'_id.a': 1}}],
    expectProjectToCoalesce: true,
    expectedCoalescedProjects: [{"_id": {"a": true}}],
    index: indexSpec,
    pipelineOptimizedAway: true,
    removedProjectStage: {'_id.a': 1},
});
