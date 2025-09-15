/*
 * Common properties our property-based tests may use. Intended to be paired with the `testProperty`
 * interface in property_testing_utils.js.
 */
import {runDeoptimized} from "jstests/libs/property_test_helpers/property_testing_utils.js";

// Returns different query shapes using the first parameters plugged in.
export function getDifferentlyShapedQueries(getQuery, testHelpers) {
    const queries = [];
    for (let queryIx = 0; queryIx < testHelpers.numQueryShapes; queryIx++) {
        queries.push(getQuery(queryIx, 0 /* paramIx */));
    }
    return queries;
}

// Using the given shapeIx, returns all variations of that shape with different parameters plugged
// in.
function getAllVariationsOfQueryShape(shapeIx, getQuery, testHelpers) {
    const queries = [];
    for (let paramIx = 0; paramIx < testHelpers.leafParametersPerFamily; paramIx++) {
        queries.push(getQuery(shapeIx, paramIx));
    }
    return queries;
}

/*
 * Runs one of each query shape with the first parameters plugged in, comparing the experiment
 * results to the control results.
 * The `statsCollectorFn`, if provided, is run on the explain of each query on the experiment
 * collection.
 */
export function createCorrectnessProperty(controlColl, experimentColl, statsCollectorFn) {
    return function queryHasSameResultsAsControlCollScan(getQuery, testHelpers) {
        const queries = getDifferentlyShapedQueries(getQuery, testHelpers);

        // Compute the control results all at once.
        const resultMap = runDeoptimized(controlColl, queries);

        for (let i = 0; i < queries.length; i++) {
            const query = queries[i];
            const controlResults = resultMap[i];
            const experimentResults = experimentColl.aggregate(query).toArray();

            if (statsCollectorFn) {
                statsCollectorFn(experimentColl.explain().aggregate(query));
            }

            if (!testHelpers.comp(controlResults, experimentResults)) {
                return {
                    passed: false,
                    message:
                        'Query results from experiment collection did not match plain collection using collscan.',
                    query,
                    explain: experimentColl.explain().aggregate(query),
                    controlResults,
                    experimentResults
                };
            }
        }
        return {passed: true};
    };
}

/*
 * Runs similar query shapes with different parameters plugged in to trigger the plan cache, and
 * compares to control results.
 * The `statsCollectorFn`, if provided, is run on the explain of each query on the experiment
 * collection.
 */
export function createCacheCorrectnessProperty(controlColl, experimentColl, statsCollectorFn) {
    return function queriesUsingCacheHaveSameResultsAsControl(getQuery, testHelpers) {
        // The first query we have available for each query shape.
        const firstQueryOfEachShape = [];
        // The rest of the queries we have available for each shape.
        const remainingQueries = [];
        for (let shapeIx = 0; shapeIx < testHelpers.numQueryShapes; shapeIx++) {
            const variations = getAllVariationsOfQueryShape(shapeIx, getQuery, testHelpers);
            firstQueryOfEachShape.push(variations[0]);
            remainingQueries.push(...variations.slice(1));
        }

        // Compute the control results all at once.
        const resultMap = runDeoptimized(controlColl, remainingQueries);

        // Run the first of each shape three times to get them cached.
        firstQueryOfEachShape.forEach(query => {
            for (let i = 0; i < 3; i++) {
                experimentColl.aggregate(query).toArray();
            }
        });

        // Check that remaining queries, with different parameters, have correct results. These
        // queries won't always use the cached plan because we don't model our
        // autoparameterization rules exactly, but that's okay.
        for (let i = 0; i < remainingQueries.length; i++) {
            const query = remainingQueries[i];
            const controlResults = resultMap[i];
            const experimentResults = experimentColl.aggregate(query).toArray();

            if (statsCollectorFn) {
                statsCollectorFn(experimentColl.explain().aggregate(query));
            }

            if (!testHelpers.comp(controlResults, experimentResults)) {
                return {
                    passed: false,
                    message: 'A query potentially using the plan cache has incorrect results. ' +
                        'The query that created the cache entry likely has different parameters.',
                    query,
                    explain: experimentColl.explain().aggregate(query),
                    controlResults,
                    experimentResults
                };
            }
        }

        return {passed: true};
    };
}
