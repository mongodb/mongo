/**
 * A property-based test that asserts the correctness of queries that begin with $match.
 *
 * @tags: [
 * query_intensive_pbt,
 * requires_timeseries,
 * # Runs queries that may return many results, requiring getmores.
 * requires_getmore,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * ]
 */

import {createCorrectnessProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {matchFirstStageAggModel} from "jstests/libs/property_test_helpers/common_models.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const is83orAbove = (() => {
    const {version} = db.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}).featureCompatibilityVersion;
    return MongoRunner.compareBinVersions(version, "8.3") >= 0;
})();

const numRuns = 40;
const numQueriesPerRun = 40;

const controlColl = db[`${jsTestName()}_control`];
const experimentColl = db[`${jsTestName()}_experiment`];

const correctnessProperty = createCorrectnessProperty(controlColl, experimentColl);
const aggModel = matchFirstStageAggModel({isTS: true, is83orAbove: is83orAbove});

testProperty(
    correctnessProperty,
    {controlColl, experimentColl},
    makeWorkloadModel({collModel: getCollectionModel({isTS: true}), aggModel, numQueriesPerRun}),
    numRuns,
);
