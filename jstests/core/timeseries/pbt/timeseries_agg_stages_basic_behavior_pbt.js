/**
 * Test basic properties that should hold for our core agg stages, when placed at the end of a
 * pipeline. This includes:
 *  - An exclusion projection should drop the specified fields.
 *  - An inclusion projection should keep the specified fields, and drop all others.
 *  - $limit should limit the number of results.
 *  - $sort should output documents in sorted order.
 *  TODO SERVER-83072 enable $group once the timeseries bug array bug is fixed.
 *  - $group should output documents with unique _ids (the group key).
 *
 * These may seem like simple checks that aren't worth testing. However with complex optimizations,
 * they may break sometimes.
 *
 * @tags: [
 * query_intensive_pbt,
 * requires_timeseries,
 * assumes_no_implicit_collection_creation_on_get_collection,
 * # Runs queries that may return many results, requiring getmores.
 * requires_getmore,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * ]
 */

import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {
    getAggPipelineArb,
    getSingleFieldProjectArb,
    getSortArb,
    limitArb,
} from "jstests/libs/property_test_helpers/models/query_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {
    checkExclusionProjectionFieldResults,
    checkInclusionProjectionResults,
    checkLimitResults,
    checkSortResults,
    makeBehavioralPropertyFn,
} from "jstests/libs/property_test_helpers/common_properties.js";
import {getNestedProperties} from "jstests/libs/query/analyze_plan.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const is83orAbove = (() => {
    const {version} = db.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}).featureCompatibilityVersion;
    return MongoRunner.compareBinVersions(version, "8.3") >= 0;
})();

const numRuns = 20;

// --- Exclusion projection testing ---
const exclusionProjectionTest = {
    // The stage we're testing.
    stageArb: getSingleFieldProjectArb(false /*isInclusion*/, {simpleFieldsOnly: true}), // Only allow simple paths, no dotted paths.
    // A function that tests the results are as expected.
    checkResultsFn: checkExclusionProjectionFieldResults,
    // A message to output on failure.
    failMsg: "Exclusion projection did not remove the specified fields.",
};

// --- Inclusion projection testing ---
const inclusionProjectionTest = {
    stageArb: getSingleFieldProjectArb(true /*isInclusion*/, {simpleFieldsOnly: true}),
    checkResultsFn: checkInclusionProjectionResults,
    failMsg: "Inclusion projection did not drop all other fields.",
};

// --- $limit testing ---
const limitTest = {
    stageArb: limitArb,
    checkResultsFn: checkLimitResults,
    failMsg: "$limit did not limit how many documents there were in the output",
};

// --- $sort testing ---
const sortTest = {
    stageArb: getSortArb(),
    checkResultsFn: checkSortResults,
    failMsg: "$sort did not output documents in sorted order.",
};

// TODO SERVER-114750 add more test cases here, or add a new PBT.

const testCases = [exclusionProjectionTest, inclusionProjectionTest, limitTest, sortTest];
const experimentColl = db[`${jsTestName()}_experiment`];

for (const {stageArb, checkResultsFn, failMsg} of testCases) {
    const propFn = makeBehavioralPropertyFn(experimentColl, checkResultsFn, failMsg);

    // Create an agg model that ends with the stage we're testing. The bag does not have to be
    // deterministic because these properties should always hold.
    const startOfPipelineArb = getAggPipelineArb({deterministicBag: false, isTS: true});
    const aggModel = fc
        .record({startOfPipeline: startOfPipelineArb, lastStage: stageArb})
        .filter(({startOfPipeline, _}) => {
            // Older versions suffer from SERVER-112844
            return is83orAbove || getNestedProperties(startOfPipeline, "$elemMatch").length == 0;
        })
        .map(function ({startOfPipeline, lastStage}) {
            return {"pipeline": [...startOfPipeline, lastStage], "options": {}};
        });

    // Run the property with a TS collection.
    testProperty(
        propFn,
        {experimentColl},
        makeWorkloadModel({collModel: getCollectionModel({isTS: true}), aggModel, numQueriesPerRun: 20}),
        numRuns,
    );
}
