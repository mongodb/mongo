/**
 * A property-based test that runs random queries with indexes (and the plan cache enabled) and
 * compares the results to the same queries, deoptimized.
 *
 * @tags: [
 * query_intensive_pbt,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * requires_timeseries,
 * assumes_no_implicit_collection_creation_on_get_collection,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * ]
 */
import {isSlowBuild} from "jstests/libs/aggregation_pipeline_utils.js";
import {createCorrectnessProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getAggPipelineModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";

if (isSlowBuild(db)) {
    jsTestLog("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 50;
const numQueriesPerRun = 20;

const controlColl = db.index_correctness_pbt_control;
const experimentColl = db.index_correctness_pbt_experiment;
const correctnessProperty = createCorrectnessProperty(controlColl, experimentColl);
const aggModel = getAggPipelineModel();

// Test with a regular collection.
testProperty(correctnessProperty,
             {controlColl, experimentColl},
             makeWorkloadModel({collModel: getCollectionModel(), aggModel, numQueriesPerRun}),
             numRuns);

// TODO SERVER-103381 re-enable timeseries PBT testing.
// Test with a TS collection.
// TODO SERVER-83072 re-enable $group in this test, by removing the filter below.
// const tsAggModel = aggModel.filter(query => {
//     for (const stage of query) {
//         if (Object.keys(stage).includes('$group')) {
//             return false;
//         }
//     }
//     return true;
// });
// testProperty(
//     correctnessProperty,
//     {controlColl, experimentColl},
//     makeWorkloadModel(
//         {collModel: getCollectionModel({isTS: true}), aggModel: tsAggModel, numQueriesPerRun}),
//     numRuns);
