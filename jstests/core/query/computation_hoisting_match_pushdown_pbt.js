/**
 * A property-based test that asserts correctness of match pushdown with computation hoisting.
 * When $project/$set/$addFields stages that don't read from a preceding $lookup's output
 * are followed by a $match, the optimizer is allowed to hoist the computation (and push the match)
 * before the $lookup. This test verifies that such rewrites produce the same results as executing
 * the original unoptimized pipeline.
 *
 * @tags: [
 *   query_intensive_pbt,
 *   requires_getmore,
 *   featureFlagImprovedDepsAnalysis,
 *   # Uses a knob (internalQueryTransformHoistPolicy) that does not exist on older binaries.
 *   multiversion_incompatible,
 *   assumes_unsharded_collection,
 *   exclude_from_timeseries_crud_passthrough,
 * ]
 */

import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {getMatchArb} from "jstests/libs/property_test_helpers/models/match_models.js";
import {dottedDollarFieldArb, dottedFieldArb, intArb} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {createQueriesWithKnobsSetAreSameAsControlCollScanProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Exiting early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 50;
const numQueriesPerRun = 200;

const controlColl = db.computation_hoisting_match_pushdown_pbt_control;
const experimentColl = db.computation_hoisting_match_pushdown_pbt_experiment;

// Collection for $lookup, with only a single document.
const lookupColl = db.computation_hoisting_match_pushdown_pbt_secondary;
lookupColl.drop();
lookupColl.insertOne({});

// We use $multiply for the expression. $convert ensures we get a numeric input.
// Prime numbers should make it less likely that the result is correct due to chance in case of an incorrect rewrite.
const safeFieldArb = dottedDollarFieldArb.map((f) => ({
    $convert: {input: f, to: "double", onError: 11, onNull: 13},
}));
const exprArb = fc.oneof(
    {arbitrary: intArb.map((i) => ({$literal: i})), weight: 1},
    {
        arbitrary: fc.tuple(safeFieldArb, fc.constant(17)).map(([f, c]) => ({$multiply: [f, c]})),
        weight: 2,
    },
    {
        arbitrary: fc.tuple(safeFieldArb, safeFieldArb).map(([l, r]) => ({$multiply: [l, r]})),
        weight: 2,
    },
);

const lookupStage = {$lookup: {from: lookupColl.getName(), pipeline: [], as: "a"}};

// Generate 1-2 dotted fields with distinct base fields.
const distinctBaseFieldsArb = fc.uniqueArray(dottedFieldArb, {
    minLength: 1,
    maxLength: 2,
    selector: (f) => f.split(".")[0],
});

const computationStageArb = fc
    .tuple(
        fc.oneof(fc.constant("$set"), fc.constant("$addFields"), fc.constant("$project")),
        distinctBaseFieldsArb,
        fc.array(exprArb, {minLength: 2, maxLength: 2}),
    )
    .map(([stageType, fields, exprs]) => ({
        [stageType]: Object.fromEntries(fields.map((f, i) => [f, exprs[i]])),
    }));

const aggModel = fc
    .tuple(
        fc.array(fc.constant(lookupStage), {minLength: 1, maxLength: 2}),
        fc.array(computationStageArb, {minLength: 1, maxLength: 2}),
        getMatchArb(),
    )
    .map(([lookups, computations, match]) => ({pipeline: [...lookups, ...computations, match], options: {}}));

const knobToVal = {internalQueryTransformHoistPolicy: "forMatchPushdown"};
const correctnessProperty = createQueriesWithKnobsSetAreSameAsControlCollScanProperty(controlColl, experimentColl);

const workloadModel = makeWorkloadModel({
    collModel: getCollectionModel({isTS: false}),
    aggModel,
    numQueriesPerRun,
    extraParamsModel: fc.constant({knobToVal}),
});

testProperty(correctnessProperty, {controlColl, experimentColl}, workloadModel, numRuns);

if (!TestData.inEvergreen) {
    // Check if we have a $project/$set/$addFields before a $match, both before a $lookup.
    function didOptimizationFire(explain) {
        const stages = explain?.stages ?? [];
        const lookupIdx = stages.findIndex((s) => "$lookup" in s);
        const prefix = stages.slice(0, lookupIdx === -1 ? stages.length : lookupIdx);
        const matchIdx = prefix.findLastIndex((s) => "$match" in s);
        return (
            matchIdx !== -1 &&
            prefix.slice(0, matchIdx).some((s) => "$project" in s || "$set" in s || "$addFields" in s)
        );
    }

    // Estimate how often the rewrite fires using independent samples from aggModel.
    // Done separately to avoid breaking transaction tests.
    const samples = fc.sample(aggModel, 500);
    let optimizationFiredRuns = 0;
    runWithParamsAllNonConfigNodes(experimentColl.getDB(), knobToVal, () => {
        for (const {pipeline, options} of samples) {
            if (didOptimizationFire(experimentColl.explain().aggregate([...pipeline], options))) {
                optimizationFiredRuns++;
            }
        }
    });
    jsTest.log.info(`Computation hoisting fired in ${(optimizationFiredRuns / samples.length) * 100}% of samples.`);
}
