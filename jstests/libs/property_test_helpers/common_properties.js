/*
 * Common properties our property-based tests may use. Intended to be paired with the `testProperty`
 * interface in property_testing_utils.js.
 */
import {getPlanCache, runDeoptimized} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {
    getAllPlans,
    getAllPlanStages,
    getPlanStages,
    getRejectedPlans,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

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
 * results to the control results. "Control" queries are collection scans run without optimizations
 * and without the plan cache.
 *
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

            let experimentResults = [];
            assert.eq(typeof query, "object");
            experimentResults = experimentColl.aggregate(query.pipeline, query.options).toArray();
            if (statsCollectorFn) {
                statsCollectorFn(experimentColl.explain().aggregate(query.pipeline, query.options));
            }

            if (!testHelpers.comp(controlResults, experimentResults)) {
                return {
                    passed: false,
                    message: "Query results from experiment collection did not match plain collection using collscan.",
                    query,
                    explain: experimentColl.explain().aggregate(query.pipeline, query.options),
                    controlResults,
                    experimentResults,
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
        firstQueryOfEachShape.forEach((query) => {
            for (let i = 0; i < 3; i++) {
                experimentColl.aggregate(query.pipeline, query.options).toArray();
            }
        });

        // Check that remaining queries, with different parameters, have correct results. These
        // queries won't always use the cached plan because we don't model our
        // autoparameterization rules exactly, but that's okay.
        for (let i = 0; i < remainingQueries.length; i++) {
            const query = remainingQueries[i];
            const controlResults = resultMap[i];
            const experimentResults = experimentColl.aggregate(query.pipeline, query.options).toArray();

            if (statsCollectorFn) {
                statsCollectorFn(experimentColl.explain().aggregate(query.pipeline, query.options));
            }

            if (!testHelpers.comp(controlResults, experimentResults)) {
                return {
                    passed: false,
                    message:
                        "A query potentially using the plan cache has incorrect results. " +
                        "The query that created the cache entry likely has different parameters.",
                    query,
                    explain: experimentColl.explain().aggregate(query.pipeline, query.options),
                    controlResults,
                    experimentResults,
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
    if (
        getPlanStages(explain, "GROUP") ||
        getPlanStages(explain, "COUNT_SCAN") ||
        getPlanStages(explain, "DISTINCT_SCAN")
    ) {
        return;
    }

    for (const plan of plans) {
        for (const stage of getAllPlanStages(plan)) {
            assert(stage.costEstimate !== undefined && stage.cardinalityEstimate !== undefined, {explain, stage});
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
    return (
        cmp(getWinningPlanFromExplain(explain1), getWinningPlanFromExplain(explain2)) &&
        cmp(getRejectedPlans(explain1), getRejectedPlans(explain2))
    );
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
            const initialExplain = experimentColl.explain().aggregate(query.pipeline, query.options);
            if (assertCeExists) {
                assertCeIsDefined(initialExplain);
            }

            for (let i = 0; i < 10; i++) {
                const newExplain = experimentColl.explain().aggregate(query.pipeline, query.options);
                if (!sameWinningAndRejectedPlans(initialExplain, newExplain)) {
                    return {
                        passed: false,
                        message:
                            "A query was found to have unstable plan selection across runs with the same documents and indexes.",
                        initialExplain,
                        newExplain,
                    };
                }
            }
        }
        return {passed: true};
    };
}

function runSetParamCommand(db, cmd) {
    FixtureHelpers.runCommandOnAllShards({db: db.getSiblingDB("admin"), cmdObj: cmd});
}

/*
 * Runs the given function with the query knobs set, then sets the query knobs back to their
 * original state.
 * It's important that each run of the property is independent from one another, so we'll always
 * reset the knobs to their original state even if the function throws an exception.
 */
function runWithKnobs(db, knobToVal, fn) {
    const knobNames = Object.keys(knobToVal);
    // If there are no knobs to change, return the result of the function since there's no other
    // work to do.
    if (knobNames.length === 0) {
        return fn();
    }

    // Get the previous knob settings, so we can undo our changes after setting the knobs from
    // `knobToVal`.
    const getParamObj = {getParameter: 1};
    for (const key of knobNames) {
        getParamObj[key] = 1;
    }
    const getParamResult = assert.commandWorked(db.adminCommand(getParamObj));
    // Copy only the knob key/vals into the new object.
    const priorSettings = {};
    for (const key of knobNames) {
        priorSettings[key] = getParamResult[key];
    }

    // Set the requested knobs.
    runSetParamCommand(db, {setParameter: 1, ...knobToVal});

    // With the finally block, we'll always revert the parameters back to their original settings,
    // even if an exception is thrown.
    try {
        return fn();
    } finally {
        // Reset to the original settings.
        runSetParamCommand(db, {setParameter: 1, ...priorSettings});
    }
}

export function createQueriesWithKnobsSetAreSameAsControlCollScanProperty(controlColl, experimentColl) {
    return function queriesWithKnobsSetAreSameAsControlCollScan(getQuery, testHelpers, {knobToVal}) {
        const queries = getDifferentlyShapedQueries(getQuery, testHelpers);

        // Compute the control results all at once.
        const resultMap = runDeoptimized(controlColl, queries);

        return runWithKnobs(experimentColl.getDB(), knobToVal, () => {
            for (let i = 0; i < queries.length; i++) {
                const query = queries[i];
                const controlResults = resultMap[i];
                const experimentResults = experimentColl.aggregate(query.pipeline, query.options).toArray();
                if (!testHelpers.comp(controlResults, experimentResults)) {
                    return {
                        passed: false,
                        message:
                            "A query with different knobs set has returned incorrect results compared to a collection scan query with no knobs set.",
                        query,
                        explain: experimentColl.explain().aggregate(query.pipeline, query.options),
                        controlResults,
                        experimentResults,
                        knobToVal,
                    };
                }
            }
            return {passed: true};
        });
    };
}

// Motivation: Check that the plan cache key we use to lookup in the cache and to store in the cache
// are consistent.
export function createRepeatQueriesUseCacheProperty(experimentColl) {
    return function repeatQueriesUseCacheProperty(getQuery, testHelpers) {
        for (let queryIx = 0; queryIx < testHelpers.numQueryShapes; queryIx++) {
            const query = getQuery(queryIx, 0 /* paramIx */);
            const explain = experimentColl.explain().aggregate(query.pipeline, query.options);

            // If there are no rejected plans, there is no need to cache.
            if (getRejectedPlans(explain).length === 0) {
                continue;
            }

            // Currently, both classic and SBE queries use the classic plan cache.
            const serverStatusBefore = db.serverStatus();
            const classicHitsBefore = serverStatusBefore.metrics.query.planCache.classic.hits;
            const sbeHitsBefore = serverStatusBefore.metrics.query.planCache.sbe.hits;

            for (let i = 0; i < 5; i++) {
                experimentColl.aggregate(query.pipeline, query.options).toArray();
            }

            const serverStatusAfter = db.serverStatus();
            const classicHitsAfter = serverStatusAfter.metrics.query.planCache.classic.hits;
            const sbeHitsAfter = serverStatusAfter.metrics.query.planCache.sbe.hits;

            // If neither the SBE plan cache hits nor the classic plan cache hits have incremented, then
            // our query must not have hit the cache. We check for at least one hit, since ties can
            // prevent a plan from being cached right away.
            if (checkSbeFullyEnabled(db) && sbeHitsAfter - sbeHitsBefore > 0) {
                continue;
            } else if (classicHitsAfter - classicHitsBefore > 0) {
                continue;
            }
            return {
                passed: false,
                message: "Plan cache hits failed to increment after running query several times.",
                query,
                explain,
                classicHitsBefore,
                classicHitsAfter,
                sbeHitsBefore,
                sbeHitsAfter,
                planCacheState: getPlanCache(experimentColl).list(),
            };
        }
        return {passed: true};
    };
}

// Function to verify the field excluded by an exclusion projection does not exist in any result documents.
export function checkExclusionProjectionFieldResults(query, results) {
    const projectSpec = query.pipeline.at(-1)["$project"];
    const excludedField = Object.keys(projectSpec).filter((field) => field !== "_id")[0];
    const isIdFieldIncluded = projectSpec._id;

    for (const doc of results) {
        const docFields = Object.keys(doc);
        // If the excluded field still exists, fail.
        if (docFields.includes(excludedField)) {
            return false;
        }
        // If _id is excluded and it exists, fail.
        if (!isIdFieldIncluded && docFields.includes("_id")) {
            return false;
        }
    }
    return true;
}

// Function to verify only the field included by an inclusion projection exists in the result documents.
export function checkInclusionProjectionResults(query, results) {
    const projectSpec = query.pipeline.at(-1)["$project"];
    const includedField = Object.keys(projectSpec).filter((field) => field !== "_id")[0];
    const isIdFieldExcluded = !projectSpec._id;

    for (const doc of results) {
        for (const field of Object.keys(doc)) {
            // If the _id field is excluded and it exists, fail.
            if (field === "_id" && isIdFieldExcluded) {
                return false;
            }
            // If we have a field on the doc that is not the included field, fail.
            if (field !== "_id" && field !== includedField) {
                return false;
            }
        }
    }
    return true;
}

// Function to verify that the number of results is less than or equal to the limit specified.
export function checkLimitResults(query, results) {
    const limitStage = query.pipeline.at(-1);
    const limitVal = limitStage["$limit"];

    return results.length <= limitVal;
}

// Function to verify that the results are sorted according to the $sort specification.
export function checkSortResults(query, results) {
    const sortSpec = query.pipeline.at(-1)["$sort"];
    const sortField = Object.keys(sortSpec)[0];
    const sortDirection = sortSpec[sortField];

    function orderCorrect(doc1, doc2) {
        const doc1SortVal = doc1[sortField];
        const doc2SortVal = doc2[sortField];

        // bsonWoCompare does not match the $sort semantics for arrays. It is nontrivial to write a
        // comparison function that matches these semantics, so we will ignore arrays.
        // TODO SERVER-101149 improve sort checking logic to possibly handle arrays and missing
        // values.
        if (Array.isArray(doc1SortVal) || Array.isArray(doc2SortVal)) {
            return true;
        }
        if (typeof doc1SortVal === "undefined" || typeof doc2SortVal === "undefined") {
            return true;
        }

        const cmp = bsonWoCompare(doc1SortVal, doc2SortVal);
        if (sortDirection === 1) {
            return cmp <= 0;
        } else {
            return cmp >= 0;
        }
    }

    for (let i = 0; i < results.length - 1; i++) {
        const doc1 = results[i];
        const doc2 = results[i + 1];
        if (!orderCorrect(doc1, doc2)) {
            return false;
        }
    }
    return true;
}

// 'checkResultsFn' takes the query and the results and outputs a boolean. Use
// 'makeBehavioralPropertyFn' when the expected behavior of a query is testable from the results
// alone. For example if we have {$limit: 2}, we can test the limit worked from the results alone,
// by asserting results.length <= 2
export function makeBehavioralPropertyFn(experimentColl, checkResultsFn, failMsg) {
    return function (getQuery, testHelpers) {
        for (let queryIx = 0; queryIx < testHelpers.numQueryShapes; queryIx++) {
            const query = getQuery(queryIx, 0 /* paramIx */);
            const results = experimentColl.aggregate(query.pipeline, query.options).toArray();

            const passed = checkResultsFn(query, results);
            if (!passed) {
                return {
                    passed: false,
                    msg: failMsg,
                    query,
                    results,
                    explain: experimentColl.explain().aggregate(query.pipeline, query.options),
                };
            }
        }
        return {passed: true};
    };
}
