/**
 * Runs a property-based test to check that queries without ORs create one plan cache entry at most.
 * This was written in case non-determinism is accidentally introduced into our plan cache key
 * calculation.
 *
 * @tags: [
 *   requires_timeseries,
 *   assumes_standalone_mongod,
 *   # Plan cache state is node-local and will not get migrated alongside user data
 *   assumes_balancer_off,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   # Need to clear cache between runs.
 *   does_not_support_stepdowns
 * ]
 */
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getAggPipelineModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {
    getPlanCache,
    testProperty
} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";

let numRuns = 200;
if (isSlowBuild(db)) {
    numRuns = 20;
    jsTestLog('Trying less examples because debug is on, opt is off, or a sanitizer is enabled.');
}
const numQueriesPerRun = 20;

const experimentColl = db[jsTestName()];

/*
 * For each different query shape, check that it creates one cache entry at most when run multiple
 * times.
 */
function identicalQueryCreatesAtMostOneCacheEntry(getQuery, testHelpers) {
    for (let queryIx = 0; queryIx < testHelpers.numQueryShapes; queryIx++) {
        const query = getQuery(queryIx, 0 /* paramIx */);
        const cacheBefore = getPlanCache(experimentColl).list();
        for (let i = 0; i < 5; i++) {
            experimentColl.aggregate(query).toArray();
        }
        const cacheAfter = getPlanCache(experimentColl).list();

        // Check that we did not create 2 or more cache entries.
        if (cacheAfter.length - cacheBefore.length >= 2) {
            return {
                passed: false,
                query,
                explain: experimentColl.explain().aggregate(query),
                cacheBefore,
                cacheAfter,
                numberOfCacheEntriesCreated: cacheAfter.length - cacheBefore.length
            };
        }
    }
    return {passed: true};
}

const aggModel = getAggPipelineModel({allowOrs: false});

testProperty(identicalQueryCreatesAtMostOneCacheEntry,
             {experimentColl},
             {collModel: getCollectionModel({isTS: false}), aggModel},
             {numRuns, numQueriesPerRun});
testProperty(identicalQueryCreatesAtMostOneCacheEntry,
             {experimentColl},
             {collModel: getCollectionModel({isTS: true}), aggModel},
             {numRuns, numQueriesPerRun});