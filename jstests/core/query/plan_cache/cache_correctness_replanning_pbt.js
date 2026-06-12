/**
 * A property-based test that populates the plan cache with a set of query shapes,
 * then replaces all documents in the collections with new documents to trigger new
 * winning plans. The query shapes are then rerun with forced replanning to ensure
 * correctness.
 *
 * @tags: [
 * query_intensive_pbt,
 * # This test runs commands that are not allowed with security token: setParameter
 * not_allowed_with_signed_security_token,
 * assumes_no_implicit_collection_creation_after_drop,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Runs queries that may return many results, requiring getmores.
 * requires_getmore,
 * # Failpoints to trigger replanning only exist on 9.0+
 * requires_fcv_90,
 * ]
 */
import {isFCVgte} from "jstests/libs/feature_compatibility_version.js";
import {createReplanningCacheCorrectnessProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getDatasetModel} from "jstests/libs/property_test_helpers/models/document_models.js";
import {getQueryAndOptionsModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {getIndexesModel} from "jstests/libs/property_test_helpers/models/index_models.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const is83orAbove = isFCVgte(db, "8.3");

const numRuns = 20;
const numQueriesPerRun = 15;

const controlColl = db.cache_correctness_replanning_pbt_control;
const experimentColl = db.cache_correctness_replanning_pbt_experiment;
const correctnessProperty = createReplanningCacheCorrectnessProperty(controlColl, experimentColl);

const indexesModel = getIndexesModel({
    isTS: TestData.isTimeseriesTestSuite,
    minNumIndexes: 10,
    maxNumIndexes: 30,
});
const aggModel = getQueryAndOptionsModel().filter(
    // Older versions suffer from SERVER-101007
    ({pipeline}) => is83orAbove || !JSON.stringify(pipeline).includes('"$elemMatch"'),
);

testProperty(
    correctnessProperty,
    {controlColl, experimentColl},
    makeWorkloadModel({
        collModel: getCollectionModel({indexesModel}),
        aggModel,
        numQueriesPerRun,
        // Pass an additional set of documents to the property to replace the first set and
        // trigger a different winning plan.
        extraParamsModel: fc.record({extraDocs: getDatasetModel()}),
    }),
    numRuns,
);
