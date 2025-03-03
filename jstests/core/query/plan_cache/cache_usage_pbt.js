/**
 * A property-based test that runs the same query several times to assert that it eventually uses
 * the plan cache.
 * There have been issues where the key we use to lookup in the plan cache is different from the
 * key we use to store the cache entry. This test attempts to target these potential bugs.
 *
 * @tags: [
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
import {getAggPipelineModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {
    getPlanCache,
    testProperty
} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {getRejectedPlans} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

let numRuns = 200;
if (isSlowBuild(db)) {
    numRuns = 20;
    jsTestLog('Trying less examples because debug is on, opt is off, or a sanitizer is enabled.');
}
const numQueriesPerRun = 20;

const experimentColl = db[jsTestName()];

// Motivation: Check that the plan cache key we use to lookup in the cache and to store in the cache
// are consistent.
function repeatQueriesUseCache(getQuery, testHelpers) {
    for (let queryIx = 0; queryIx < testHelpers.numQueryShapes; queryIx++) {
        const query = getQuery(queryIx, 0 /* paramIx */);
        const explain = experimentColl.explain().aggregate(query);
        // If there are no rejected plans, there is no need to cache.
        if (getRejectedPlans(explain).length === 0) {
            return {passed: true};
        }

        const firstResult = experimentColl.aggregate(query).toArray();

        // Currently, both classic and SBE queries use the classic plan cache.
        const serverStatusBefore = db.serverStatus();
        const classicHitsBefore = serverStatusBefore.metrics.query.planCache.classic.hits;
        const sbeHitsBefore = serverStatusBefore.metrics.query.planCache.sbe.hits;

        for (let i = 0; i < 5; i++) {
            experimentColl.aggregate(query).toArray();
        }

        const serverStatusAfter = db.serverStatus();
        const classicHitsAfter = serverStatusAfter.metrics.query.planCache.classic.hits;
        const sbeHitsAfter = serverStatusAfter.metrics.query.planCache.sbe.hits;
        // If neither the SBE plan cache hits nor the classic plan cache hits have incremented, then
        // our query must not have hit the cache. We check for at least one hit, since ties can
        // prevent a plan from being cached right away.
        if (checkSbeFullyEnabled(db) && sbeHitsAfter - sbeHitsBefore > 0) {
            return {passed: true};
        } else if (classicHitsAfter - classicHitsBefore > 0) {
            return {passed: true};
        }
        return {
            passed: false,
            message: 'Plan cache hits failed to increment after running query several times.',
            query,
            explain,
            classicHitsBefore,
            classicHitsAfter,
            sbeHitsBefore,
            sbeHitsAfter,
            planCacheState: getPlanCache(experimentColl).list()
        };
    }
    return {passed: true};
}

const aggModel = getAggPipelineModel();

testProperty(repeatQueriesUseCache,
             {experimentColl},
             {collModel: getCollectionModel({isTS: false}), aggModel},
             {numRuns, numQueriesPerRun});
testProperty(repeatQueriesUseCache,
             {experimentColl},
             {collModel: getCollectionModel({isTS: true}), aggModel},
             {numRuns, numQueriesPerRun});