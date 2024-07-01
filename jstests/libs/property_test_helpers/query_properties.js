import {getRejectedPlans} from "jstests/libs/analyze_plan.js";
import {
    aggPipelineModel,
    aggPipelineNoOrsModel,
} from "jstests/libs/property_test_helpers/query_models.js";
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

/*
 * Properties take the collection we're testing, a list of queries to use during the property test,
 * and some helpers which include a comparator, a control collection, etc.
 */

// Motivation: Query correctness.
function queryHasSameResultsAsControlCollScan(experimentColl, queries, testHelpers) {
    for (let i = 0; i < queries.length; i++) {
        const query = queries[i];
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
function repeatQueriesReturnSameResults(experimentColl, queries, testHelpers) {
    const query = queries[0];
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
function repeatQueriesUseCache(experimentColl, queries, testHelpers) {
    const query = queries[0];

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
    if (checkSbeFullyEnabled(testHelpers.experimentDb) && sbeHitsAfter - sbeHitsBefore === 4) {
        return {passed: true};
    } else if (classicHitsAfter - classicHitsBefore === 4) {
        return {passed: true};
    }
    return {
        passed: false,
        message: 'Plan cache hits failed to increment after running query several times.',
        query,
        explain,
        classicHitsBefore,
        classicHitsAfter,
        planCacheState: testHelpers.getPlanCache(experimentColl).list()
    };
}

// Motivation: Auto-parameterization and fetching from the plan cache correctness.
function cachedQueriesHaveSameResultsAsControlCollScan(experimentColl, queries, testHelpers) {
    // Get the query shape cached.
    const initialQuery = queries[0];
    for (let i = 0; i < 3; i++) {
        experimentColl.aggregate(initialQuery).toArray();
    }

    // Check that following queries, with different parameters, have correct results. These queries
    // won't always use the cached plan because we don't model our autoparameterization rules in
    // this test, but that's okay.
    for (let i = 1; i < queries.length; i++) {
        const query = queries[i];
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

    return {passed: true};
}

// Motivation: Check that our plan cache keys are deterministic.
function identicalQueryCreatesAtMostOneCacheEntry(experimentColl, queries, testHelpers) {
    const query = queries[0];
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
        propertyFn: cachedQueriesHaveSameResultsAsControlCollScan,
        aggModel: aggPipelineModel,
        numQueriesNeeded: 10,
        numRuns: 100
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
