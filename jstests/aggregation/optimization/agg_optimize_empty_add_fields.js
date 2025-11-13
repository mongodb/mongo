/**
 * SERVER-111948 Simplify vacuous $addFields stages
 *
 * @tags: [
 *  # Required to optimize $addFields
 *  requires_pipeline_optimization,
 *  # Facets break the checks on explain()
 *  do_not_wrap_aggregations_in_facets,
 *  # Run against mongod for the explain()
 *  assumes_against_mongod_not_mongos
 * ]
 */
import {isAggregationPlan} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(
    coll.insertMany([
        {_id: 1, a: "foo"},
        {_id: 2, a: "bar"},
        {_id: 3, a: "baz"},
    ]),
);

function explainPipeline(pipeline) {
    // Add $_internalInhibitOptimization to prevent the pipeline from being optimized away
    const explain = coll.explain().aggregate([{$_internalInhibitOptimization: {}}, ...pipeline]);
    assert.eq(true, isAggregationPlan(explain), "Pipeline should not be optimized away");
    jsTest.log.info("Stages for pipeline", {input: pipeline, stages: explain.stages, explain});
    return explain;
}

const emptyAddFields = explainPipeline([{$addFields: {}}]);
// Allow the .stages to be undefined, which occurs when the pipeline is optimized to a find
assert.doesNotContain({$addFields: {}}, emptyAddFields.stages || [], "The empty $addFields stage should be removed");

const nonEmptyAddFields = explainPipeline([{$addFields: {b: 1}}]);
assert.contains(
    {$addFields: {b: {$const: 1}}},
    nonEmptyAddFields.stages,
    "Non-empty $addFields stage remains in the pipeline",
);

const removeAddFields = explainPipeline([{$addFields: {b: "$$REMOVE"}}]);
assert.contains(
    {$addFields: {b: "$$REMOVE"}},
    removeAddFields.stages,
    "$addFields which only removes fields is not optimized away",
);
