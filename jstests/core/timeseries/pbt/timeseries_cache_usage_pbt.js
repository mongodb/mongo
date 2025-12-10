/**
 * A property-based test that runs the same query on a timeseries collection several times to assert
 * that it eventually uses the plan cache.
 * There have been issues where the key we use to lookup in the plan cache is different from the
 * key we use to store the cache entry. This test attempts to target these potential bugs.
 *
 * @tags: [
 * query_intensive_pbt,
 * requires_timeseries,
 * assumes_standalone_mongod,
 * # Plan cache state is node-local and will not get migrated alongside user data
 * assumes_balancer_off,
 * assumes_no_implicit_collection_creation_after_drop,
 * # Need to clear cache between runs.
 * does_not_support_stepdowns
 * ]
 */
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getQueryAndOptionsModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {getNestedProperties} from "jstests/libs/query/analyze_plan.js";
import {createRepeatQueriesUseCacheProperty} from "jstests/libs/property_test_helpers/common_properties.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const is83orAbove = (() => {
    const {version} = db.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
    return MongoRunner.compareBinVersions(version, "8.3") >= 0;
})();

const numRuns = 100;
const numQueriesPerRun = 40;

const experimentColl = db[jsTestName()];

const aggModel = getQueryAndOptionsModel({isTS: true}).filter(
    // Older versions suffer from SERVER-101007
    ({pipeline}) => is83orAbove || getNestedProperties(pipeline, "$elemMatch").length == 0,
);

testProperty(
    createRepeatQueriesUseCacheProperty(experimentColl),
    {experimentColl},
    makeWorkloadModel({collModel: getCollectionModel({isTS: true}), aggModel, numQueriesPerRun}),
    numRuns,
);
