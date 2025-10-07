/**
 * A property-based test that runs queries with random query knobs set and asserts the correctness
 * compared to a collection scan with no knobs set.
 *
 * @tags: [
 * query_intensive_pbt,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * config_shard_incompatible,
 * requires_timeseries,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * # Some query knobs may not exist on older versions.
 * multiversion_incompatible
 * ]
 */
import {
    createQueriesWithKnobsSetAreSameAsControlCollScanProperty
} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {queryKnobsModel} from "jstests/libs/property_test_helpers/models/query_knob_models.js";
import {getAggPipelineModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

if (isSlowBuild(db)) {
    jsTestLog("Exiting early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 30;
const numQueriesPerRun = 50;

const controlColl = db.query_knob_correctness_pbt_control;
const experimentColl = db.query_knob_correctness_pbt_experiment;

function getWorkloadModel(isTS, aggModel) {
    return fc
        .record({
            collSpec: getCollectionModel({isTS}),
            queries: fc.array(aggModel, {minLength: 1, maxLength: numQueriesPerRun}),
            knobToVal: queryKnobsModel
        })
        .map(({collSpec, queries, knobToVal}) => {
            return {collSpec, queries, extraParams: {knobToVal}};
        });
}

const knobCorrectnessProperty =
    createQueriesWithKnobsSetAreSameAsControlCollScanProperty(controlColl, experimentColl);

// Test with a regular collection.
testProperty(
    knobCorrectnessProperty,
    {controlColl, experimentColl},
    getWorkloadModel(false /* isTS */, getAggPipelineModel()),
    numRuns,
);

// TODO SERVER-103381 re-enable timeseries PBT testing.
// Test with a TS collection.
// TODO SERVER-83072 re-enable $group in this test, by removing the filter below.
// const tsAggModel = getAggPipelineModel().filter(query => {
//     for (const stage of query) {
//         if (Object.keys(stage).includes('$group')) {
//             return false;
//         }
//     }
//     return true;
// });
// testProperty(queriesWithKnobsSetAreSameAsControlCollScan,
//              {controlColl, experimentColl},
//              getWorkloadModel(true /* isTS */, tsAggModel),
//              numRuns);
