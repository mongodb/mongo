import {
    aggPipelineModel,
    aggPipelineNoOrsModel,
    maxNumLeafParametersPerFamily
} from "jstests/libs/property_test_helpers/query_models.js";
import {getRejectedPlans} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

/*
 * Properties take the collection we're testing, a list of query families to use during the property
 * test, and some helpers which include a comparator, a control collection, etc.
 *
 * The `getQuery(i, j)` function returns query shape `i` with it's `j`th parameters plugged in.
 * For example, to get different query shapes we would call
 *      getQuery(0, 0)
 *      getQuery(1, 0)
 *      ...
 * To get the same query shape with different parameters, we would call
 *      getQuery(0, 0)
 *      getQuery(0, 1)
 *      ...
 * TODO SERVER-98132 redesign getQuery to be more opaque about how many query shapes and constants
 * there are.
 */

// Motivation: Query correctness.
function queryHasSameResultsAsControlCollScan(experimentColl, getQuery, testHelpers) {
    for (let queryIx = 0; queryIx < testHelpers.numQueryShapes; queryIx++) {
        const query = getQuery(queryIx, 0 /* paramIx */);

        const controlResults = testHelpers.controlColl.aggregate(query).toArray();
        const experimentalResults = experimentColl.aggregate(query).toArray();
        if (!testHelpers.comp(controlResults, experimentalResults)) {
            return {
                passed: false,
                message:
                    'Query results from experiment collection did not match plain collection using collscan.',
                query,
                explain: experimentColl.explain().aggregate(query),
                controlResults,
                experimentalResults
            };
        }
    }
    return {passed: true};
}

// Motivation: Auto-parameterization and plan cache correctness.
function repeatQueriesReturnSameResults(experimentColl, getQuery, testHelpers) {
    const query = getQuery(0 /* queryIx */, 0 /* paramIx */);
    const firstResult = experimentColl.aggregate(query).toArray();
    for (let repeatRun = 0; repeatRun < 5; repeatRun++) {
        const repeatResult = experimentColl.aggregate(query).toArray();
        if (!testHelpers.comp(firstResult, repeatResult)) {
            return {
                passed: false,
                message: 'Running the same query repeatedly did not yield the same results.',
                firstResult,
                repeatResult
            };
        }
    }
    return {passed: true};
}

// Motivation: Check that the plan cache key we use to lookup in the cache and to store in the cache
// are consistent.
function repeatQueriesUseCache(experimentColl, getQuery, testHelpers) {
    const query = getQuery(0 /* queryIx */, 0 /* paramIx */);
    const explain = experimentColl.explain().aggregate(query);
    if (getRejectedPlans(explain).length === 0) {
        return {passed: true};
    }

    const firstResult = experimentColl.aggregate(query).toArray();

    // Currently, both classic and SBE queries use the classic plan cache.
    const serverStatusBefore = testHelpers.serverStatus();
    const classicHitsBefore = serverStatusBefore.metrics.query.planCache.classic.hits;
    const sbeHitsBefore = serverStatusBefore.metrics.query.planCache.sbe.hits;

    for (let i = 0; i < 5; i++) {
        experimentColl.aggregate(query).toArray();
    }

    const serverStatusAfter = testHelpers.serverStatus();
    const classicHitsAfter = serverStatusAfter.metrics.query.planCache.classic.hits;
    const sbeHitsAfter = serverStatusAfter.metrics.query.planCache.sbe.hits;
    // If neither the SBE plan cache hits nor the classic plan cache hits have incremented, then our
    // query must not have hit the cache.
    // We check for at least one hit, since ties can prevent a plan from being cached right away.
    if (checkSbeFullyEnabled(testHelpers.experimentDb) && sbeHitsAfter - sbeHitsBefore > 0) {
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
        planCacheState: testHelpers.getPlanCache(experimentColl).list()
    };
}

// Motivation: Auto-parameterization and fetching from the plan cache correctness.
function queriesUsingCacheHaveSameResultsAsControl(experimentColl, getQuery, testHelpers) {
    for (let queryShapeIx = 0; queryShapeIx < testHelpers.numQueryShapes; queryShapeIx++) {
        // Get the query shape cached.
        const initialQuery = getQuery(queryShapeIx, 0 /* paramIx */);
        for (let i = 0; i < 3; i++) {
            experimentColl.aggregate(initialQuery).toArray();
        }

        // Check that following queries, with different parameters, have correct results. These
        // queries won't always use the cached plan because we don't model our autoparameterization
        // rules in this test, but that's okay.
        for (let paramIx = 1; paramIx < maxNumLeafParametersPerFamily; paramIx++) {
            const query = getQuery(queryShapeIx, paramIx);
            const controlResults = testHelpers.controlColl.aggregate(query).toArray();
            const experimentResults = experimentColl.aggregate(query).toArray();
            if (!testHelpers.comp(controlResults, experimentResults)) {
                return {
                    passed: false,
                    message: 'A query potentially using the plan cache has incorrect results. ' +
                        'The query that created the cache entry likely has different parameters.',
                    initialQuery,
                    query,
                    explain: experimentColl.explain().aggregate(query),
                    controlResults,
                    experimentResults
                };
            }
        }
    }

    return {passed: true};
}

// Motivation: Check that our plan cache keys are deterministic.
function identicalQueryCreatesAtMostOneCacheEntry(experimentColl, getQuery, testHelpers) {
    const query = getQuery(0 /* queryIx */, 0 /* paramIx */);
    const cacheBefore = testHelpers.getPlanCache(experimentColl).list();
    for (let i = 0; i < 5; i++) {
        experimentColl.aggregate(query).toArray();
    }
    const cacheAfter = testHelpers.getPlanCache(experimentColl).list();

    if (cacheAfter.length - cacheBefore.length <= 1) {
        return {passed: true};
    }
    return {
        passed: false,
        query,
        explain: experimentColl.explain().aggregate(query),
        cacheBefore,
        cacheAfter,
        numberOfCacheEntriesCreated: cacheAfter.length - cacheBefore.length
    };
}

// A list of property tests, and the corresponding model for creating agg pipelines they use. Also
// include how many queries each property needs in order to run.
export const propertyTests = [
    {
        propertyFn: queryHasSameResultsAsControlCollScan,
        aggModel: aggPipelineModel,
        numQueriesNeeded: 10,
        numRuns: 300
    },
    {
        propertyFn: repeatQueriesReturnSameResults,
        aggModel: aggPipelineModel,
        numQueriesNeeded: 1,
        numRuns: 100
    },
    {
        propertyFn: repeatQueriesUseCache,
        aggModel: aggPipelineModel,
        numQueriesNeeded: 1,
        numRuns: 300
    },
    {
        propertyFn: queriesUsingCacheHaveSameResultsAsControl,
        aggModel: aggPipelineModel,
        numQueriesNeeded: 10,
        numRuns: 300
    },
    {
        propertyFn: identicalQueryCreatesAtMostOneCacheEntry,
        // No $or allowed for this property, since a query with an $or may lead to subplanning
        // with multiple cache entries.
        aggModel: aggPipelineNoOrsModel,
        numQueriesNeeded: 1,
        numRuns: 300
    }
];
