/**
 * Runs a property-based test to check that the 'isCached' explain field is only ever true for one
 * plan per query.
 *
 * @tags: [
 *   query_intensive_pbt,
 *   assumes_standalone_mongod,
 *   # Plan cache state is node-local and will not get migrated alongside user data
 *   assumes_balancer_off,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   # Need to clear cache between runs.
 *   does_not_support_stepdowns
 * ]
 */
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getQueryAndOptionsModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {getPlanCache, testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {getRejectedPlans, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

const experimentColl = db[jsTestName()];

// The `isCached` explain field should only be true for one plan. Only one plan can be cached per
// query shape.
function isCachedIsTrueForOnePlan(getQuery, testHelpers) {
    for (let queryIx = 0; queryIx < testHelpers.numQueryShapes; queryIx++) {
        getPlanCache(experimentColl).clear();
        const query = getQuery(queryIx, 0 /* paramIx */);
        // Get the plan cached
        for (let i = 0; i < 5; i++) {
            experimentColl.aggregate(query.pipeline, query.options).toArray();
        }
        // Inspect the explain to count how many isCached=true fields we see.
        const explain = experimentColl.explain().aggregate(query.pipeline, query.options);
        const winningPlan = getWinningPlanFromExplain(explain);
        const rejectedPlans = getRejectedPlans(explain);
        let isCachedCount = 0;
        if (winningPlan.isCached) {
            isCachedCount++;
        }
        for (const rejectedPlan of rejectedPlans) {
            if (rejectedPlan.isCached) {
                isCachedCount++;
            }
        }
        if (isCachedCount > 1) {
            return {passed: false, explain};
        }
    }
    return {passed: true};
}

const numRuns = 50;
const numQueriesPerRun = 50;

// Run the property with a regular collection.
testProperty(
    isCachedIsTrueForOnePlan,
    {experimentColl},
    makeWorkloadModel({
        collModel: getCollectionModel(),
        // A deterministic set of results isn't needed to check the `isCached` property.
        aggModel: getQueryAndOptionsModel({deterministicBag: false}),
        numQueriesPerRun,
    }),
    numRuns,
);
