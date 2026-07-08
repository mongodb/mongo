/**
 * A property-based test that runs queries with random query knobs set and asserts the correctness
 * compared to a collection scan with no knobs set.
 *
 * @tags: [
 * query_intensive_pbt,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * config_shard_incompatible,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * # Some query knobs may not exist on older versions.
 * multiversion_incompatible,
 * ]
 */
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {buildQueryKnobsModel} from "jstests/libs/property_test_helpers/models/query_knob_models.js";
import {getQueryAndOptionsModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {createQueriesWithKnobsSetAreSameAsControlCollScanProperty} from "jstests/libs/property_test_helpers/common_properties.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Exiting early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 30;
const numQueriesPerRun = 50;

const controlColl = db.query_knob_correctness_pbt_control;
const experimentColl = db.query_knob_correctness_pbt_experiment;

const excludeKnobs = [
    // Small values reject otherwise-valid multi-stage pipelines with error 7749501, which is a
    // server-side guardrail rather than a correctness divergence.
    "internalPipelineLengthLimit",
    // Small values reject otherwise-valid sub-pipelines with error 232
    // (MaxSubPipelineDepthExceeded), which is a server-side guardrail rather than a correctness
    // divergence.
    "internalMaxSubPipelineViewDepth",
    /*
     * TODO SERVER-99091 re-enable CE methods for PBT.
     * Using the knobs below runs into "Currently index union is a top-level node."
     * {
     * 	"internalQueryPlannerEnableHashIntersection" : true,
     * 	"featureFlagCostBasedRanker": true,
     * 	"internalQueryCBRCEMode" : "automaticCE"
     * }
     */
    "internalQueryCBRCEMode",
    "automaticCEPlanRankingStrategy",
    "internalQueryPlannerEnableHashIntersection",
    "internalQuerySamplingCEMethod",
    // Disallows collection scans; if the generated query has no covering index this is a
    // legitimate NoQueryExecutionPlans server-side guardrail, not a correctness divergence, so it
    // can't be compared against a forced-COLLSCAN control.
    "notablescan",
    // Non-spilling stage memory limits: small values uassert instead of returning results, a
    // server-side guardrail rather than a correctness divergence.
    "internalDocumentSourceDensifyMaxMemoryBytes",
    "internalQueryFacetBufferSizeBytes",
    "internalOrStageMaxMemoryBytes",
    "internalMergeSortStageMaxMemoryBytes",
    "internalIndexScanStageMaxMemoryBytes",
    "internalSlotBasedExecutionUniqueStageMaxMemoryBytes",
    "internalSlotBasedExecutionMergeJoinStageMaxMemoryBytes",
    "internalSlotBasedExecutionAndHashStageMaxMemoryBytes",
    "internalUpdateStageMaxMemoryBytes",
    "internalCountScanStageMaxMemoryBytes",
    // Expression-evaluation byte limits: uassert (no spill) and apply to internal pipelines.
    "internalSingleDocumentTransformationStageMaxExpressionEvaluationBytes",
    "internalMatchStageMaxExpressionEvaluationBytes",
    "internalLookupStageMaxExpressionEvaluationBytes",
    "internalRedactStageMaxExpressionEvaluationBytes",
];

const knobSchema = db
    .getSiblingDB("admin")
    .aggregate([{$listQueryKnobs: {}}, {$match: {name: {$nin: excludeKnobs}}}, {$sort: {name: 1}}])
    .toArray();

const queryKnobsModel = buildQueryKnobsModel(knobSchema);

function getWorkloadModel() {
    return fc
        .record({
            collSpec: getCollectionModel(),
            queries: fc.array(getQueryAndOptionsModel(), {
                minLength: 1,
                maxLength: numQueriesPerRun,
            }),
            knobToVal: queryKnobsModel,
        })
        .map(({collSpec, queries, knobToVal}) => {
            return {collSpec, queries, extraParams: {knobToVal}};
        });
}

const knobCorrectnessProperty = createQueriesWithKnobsSetAreSameAsControlCollScanProperty(
    controlColl,
    experimentColl,
);

// Test with a regular collection.
testProperty(knobCorrectnessProperty, {controlColl, experimentColl}, getWorkloadModel(), numRuns);
