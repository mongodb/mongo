/**
 * A property-based test that runs similar queries to potentially trigger cache usage, then asserts
 * the queries return the same results when deoptimized.
 * Auto-parameterization has had issues in the past. This test attempts to target that area.
 *
 * @tags: [
 * query_intensive_pbt,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * requires_timeseries,
 * assumes_no_implicit_collection_creation_after_drop,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * ]
 */
import {createCacheCorrectnessProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getQueryAndOptionsModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";

if (isSlowBuild(db)) {
    jsTestLog("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 50;
const numQueriesPerRun = 15;

const controlColl = db.cache_correctness_pbt_control;
const experimentColl = db.cache_correctness_pbt_experiment;
const correctnessProperty = createCacheCorrectnessProperty(controlColl, experimentColl);
const aggModel = getQueryAndOptionsModel();

// Test with a regular collection.
testProperty(
    correctnessProperty,
    {controlColl, experimentColl},
    makeWorkloadModel({collModel: getCollectionModel(), aggModel, numQueriesPerRun}),
    numRuns,
);

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
