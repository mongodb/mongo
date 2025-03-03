/**
 * A property-based test that runs similar queries to potentially trigger cache usage, then asserts
 * the queries return the same results when deoptimized.
 * Auto-parameterization has had issues in the past. This test attempts to target that area.
 *
 * @tags: [
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * requires_timeseries,
 * assumes_no_implicit_collection_creation_after_drop,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Change in read concern can slow down queries enough to hit a timeout.
 * assumes_read_concern_unchanged,
 * does_not_support_causal_consistency,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * ]
 */
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getAggPipelineModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {
    runDeoptimizedQuery,
    testProperty
} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";

let numRuns = 100;
let numQueriesPerRun = 15;
if (isSlowBuild(db)) {
    numRuns = 10;
    numQueriesPerRun = 1;
    jsTestLog('Trying less examples because debug is on, opt is off, or a sanitizer is enabled.');
}

const controlColl = db.cache_correctness_pbt_control;
const experimentColl = db.cache_correctness_pbt_experiment;

function queriesUsingCacheHaveSameResultsAsControl(getQuery, testHelpers) {
    for (let queryShapeIx = 0; queryShapeIx < testHelpers.numQueryShapes; queryShapeIx++) {
        // Get the query shape cached.
        const initialQuery = getQuery(queryShapeIx, 0 /* paramIx */);
        for (let i = 0; i < 3; i++) {
            experimentColl.aggregate(initialQuery).toArray();
        }

        // Check that following queries, with different parameters, have correct results. These
        // queries won't always use the cached plan because we don't model our autoparameterization
        // rules exactly, but that's okay.
        for (let paramIx = 1; paramIx < testHelpers.leafParametersPerFamily; paramIx++) {
            const query = getQuery(queryShapeIx, paramIx);
            const controlResults = runDeoptimizedQuery(controlColl, query);
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

const aggModel = getAggPipelineModel();

// Test with a regular collection.
testProperty(queriesUsingCacheHaveSameResultsAsControl,
             {controlColl, experimentColl},
             {collModel: getCollectionModel(), aggModel},
             {numRuns, numQueriesPerRun});

// TODO SERVER-101271 re-enable PBT testing for time-series
// // Test with a TS collection.
// // TODO SERVER-83072 re-enable $group in this test, by removing the filter below.
// const tsAggModel = aggModel.filter(query => {
//     for (const stage of query) {
//         if (Object.keys(stage).includes('$group')) {
//             return false;
//         }
//     }
//     return true;
// });
// testProperty(queriesUsingCacheHaveSameResultsAsControl,
//              {controlColl, experimentColl},
//              {collModel: getCollectionModel({isTS: true}), aggModel: tsAggModel},
//              {numRuns, numQueriesPerRun});
