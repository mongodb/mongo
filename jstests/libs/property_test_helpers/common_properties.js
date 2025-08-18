/*
 * Common properties our property-based tests may use. Intended to be paired with the `testProperty`
 * interface in property_testing_utils.js.
 */
import {runDeoptimized} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {
    getAllPlans,
    getAllPlanStages,
    getPlanStages,
    getRejectedPlans,
    getWinningPlanFromExplain
} from "jstests/libs/query/analyze_plan.js";

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

/*
 * Asserts that `costEstimate` and `cardinalityEstimate` are defined in every stage of every plan
 * in the explain.
 */
function assertCeIsDefined(explain) {
    const plans = getAllPlans(explain);
    // If GROUP, COUNT or DISTINCT stages appear, CBR bails and the fields won't appear.
    if (getPlanStages(explain, 'GROUP') || getPlanStages(explain, 'COUNT_SCAN') ||
        getPlanStages(explain, 'DISTINCT_SCAN')) {
        return;
    }

    for (const plan of plans) {
        for (const stage of getAllPlanStages(plan)) {
            assert(stage.costEstimate !== undefined && stage.cardinalityEstimate !== undefined,
                   {explain, stage});
        }
    }
}

/*
 * Checks if the winning plan is the same between explains, and the same rejected plans exist.
 */
function sameWinningAndRejectedPlans(explain1, explain2) {
    // Compare the whole plan object using friendlyEqual (this has the same behavior as our regular
    // assert.eq utils)
    const cmp = friendlyEqual;
    return cmp(getWinningPlanFromExplain(explain1), getWinningPlanFromExplain(explain2)) &&
        cmp(getRejectedPlans(explain1), getRejectedPlans(explain2));
}

/*
 * Creates a property to assert that winning and rejected plans are the same across several runs. If
 * `assertCeExists` is set, an additional assertion is made that the cardinality and cost estimate
 * fields are defined.
 */
export function createPlanStabilityProperty(experimentColl, assertCeExists = false) {
    return function planStabilityProperty(getQuery, testHelpers) {
        const queries = getDifferentlyShapedQueries(getQuery, testHelpers);

        for (const query of queries) {
            // Run explain on the query once to get the initial winning plan. Then we run explain
            // ten more times to assert that the winning plan is the same each time.
            const initialExplain = experimentColl.explain().aggregate(query);
            if (assertCeExists) {
                assertCeIsDefined(initialExplain);
            }

            for (let i = 0; i < 10; i++) {
                const newExplain = experimentColl.explain().aggregate(query);
                if (!sameWinningAndRejectedPlans(initialExplain, newExplain)) {
                    return {
                        passed: false,
                        message:
                            'A query was found to have unstable plan selection across runs with the same documents and indexes.',
                        initialExplain,
                        newExplain
                    };
                }
            }
        }
        return {passed: true};
    };
}