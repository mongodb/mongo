/**
 * Check SBE native $minN/$maxN group accumulator correctness.
 *
 * @tags: [
 *   uses_explain,
 *   requires_sbe,
 *   query_intensive_pbt,
 *   cannot_run_during_upgrade_downgrade,
 *   incompatible_aubsan,
 *   # Explain does not support stepdowns.
 *   does_not_support_stepdowns,
 *   # Explain cannot run within a multi-document transaction.
 *   does_not_support_transactions,
 *   requires_fcv_91,
 *   # This test runs commands that are not allowed with security token: setParameter.
 *   not_allowed_with_signed_security_token,
 *   # Runs queries that may return many results, requiring getmores.
 *   requires_getmore,
 *   # The implicit timeseries collection conversion changes the $group plan shape and results.
 *   exclude_from_timeseries_crud_passthrough,
 * ]
 */
import {createCorrectnessProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getDatasetModel} from "jstests/libs/property_test_helpers/models/document_models.js";
import {getIndexesModel} from "jstests/libs/property_test_helpers/models/index_models.js";
import {groupMinMaxNArb} from "jstests/libs/property_test_helpers/models/group_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {minMaxNPushdownEligibleAggModel} from "jstests/libs/property_test_helpers/common_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {getEngine, getNestedProperties, getQueryPlanners} from "jstests/libs/query/analyze_plan.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Exiting early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 10;
const numQueriesPerRun = 10;
const maxNumDocs = 50;
const maxNumIndexes = 5;

const controlColl = db[jsTestName() + "_control"];
const experimentColl = db[jsTestName() + "_experiment"];

function assertNativeMinMaxN(explain) {
    if (explain.splitPipeline) {
        const shardsPart = explain.splitPipeline.shardsPart ?? [];
        // Skip queries that don't run $group on a shard.
        fc.pre(shardsPart.some((stage) => stage.hasOwnProperty("$group")));
    }
    const stages = getQueryPlanners(explain).flatMap((qp) =>
        getNestedProperties(qp, "slotBasedPlan").flatMap((plan) => plan.stages ?? []),
    );
    const planStr = stages.join("\n");
    // Skip queries that don't use minN/maxN.
    fc.pre(
        planStr.includes("_internalMinN") ||
            planStr.includes("_internalMaxN") ||
            planStr.includes("aggMinN") ||
            planStr.includes("aggMaxN"),
    );
    assert.eq(getEngine(explain), "sbe", "Expected SBE engine", {explain});
    assert(
        planStr.includes("_internalMinN") || planStr.includes("_internalMaxN"),
        "Should use native accumulator",
        {explain, planStr},
    );
    assert(
        !planStr.includes("aggMinN") && !planStr.includes("aggMaxN"),
        "Should not use 'compiled' accumulator",
        {explain, planStr},
    );
}

// Single stage $group queries.
testProperty(
    createCorrectnessProperty(controlColl, experimentColl, {
        preconditionFn: assertNativeMinMaxN,
    }),
    {controlColl, experimentColl},
    makeWorkloadModel({
        collModel: getCollectionModel({
            docsModel: getDatasetModel({maxNumDocs}),
            indexesModel: getIndexesModel({maxNumIndexes}),
        }),
        aggModel: groupMinMaxNArb.map((groupStage) => {
            return {pipeline: [groupStage], options: {}};
        }),
        numQueriesPerRun,
    }),
    numRuns,
);

// SBE-eligible $group queries.
testProperty(
    createCorrectnessProperty(controlColl, experimentColl, {
        preconditionFn: assertNativeMinMaxN,
    }),
    {controlColl, experimentColl},
    makeWorkloadModel({
        collModel: getCollectionModel({
            docsModel: getDatasetModel({maxNumDocs}),
            indexesModel: getIndexesModel({maxNumIndexes}),
        }),
        aggModel: minMaxNPushdownEligibleAggModel(),
        numQueriesPerRun,
    }),
    numRuns,
);
