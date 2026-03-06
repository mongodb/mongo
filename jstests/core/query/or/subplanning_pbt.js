/**
 * Property-based test that asserts correctness of queries that begin with a $match with a top-level
 * $or predicate. This tests subplanning code paths which are significantly different from others.
 *
 * @tags: [
 * query_intensive_pbt,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * assumes_no_implicit_collection_creation_on_get_collection,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * ]
 */
import {createCacheCorrectnessProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {topLevelOrAggModel} from "jstests/libs/property_test_helpers/common_models.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Exiting early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const is83orAbove = (() => {
    const {version} = db.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}).featureCompatibilityVersion;
    return MongoRunner.compareBinVersions(version, "8.3") >= 0;
})();

const numRuns = 15;
const numQueriesPerRun = 20;

const controlColl = db.subplanning_pbt_control;
const experimentColl = db.subplanning_pbt_experiment;
// Use the cache correctness property, which runs similar query shapes with different constant
// values plugged in. We do this because subplanning can have unique interactions with the plan
// cache.
const correctnessProperty = createCacheCorrectnessProperty(controlColl, experimentColl);

const aggModel = topLevelOrAggModel({is83orAbove: is83orAbove});

// Test with a regular collection.
testProperty(
    correctnessProperty,
    {controlColl, experimentColl},
    makeWorkloadModel({collModel: getCollectionModel(), aggModel, numQueriesPerRun}),
    numRuns,
);
