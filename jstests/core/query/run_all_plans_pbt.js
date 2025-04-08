/**
 * A property-based test to hint every index for a query, and assert the same results are returned
 * when compared to the deoptimized query.
 * Some query plans are rarely chosen because a specific data distribution is required for them to
 * be optimal. This makes it difficult to test the correctness of all plans end-to-end, so we make a
 * best effort attempt here until SERVER-83234 is complete.
 *
 * TODO SERVER-83234
 * When hinting via QuerySolution hash is available, we'll be able to hint every _plan_, rather
 * than hinting every _index_. Currently we miss intersection and union plans among others.
 * We should be able to run explain, find all of the QSN hashes to hint, then perform the assertion
 * about all plans.
 *
 * @tags: [
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * requires_timeseries,
 * assumes_no_implicit_collection_creation_on_get_collection,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Change in read concern can slow down queries enough to hit the evergreen timeout.
 * assumes_read_concern_unchanged,
 * does_not_support_causal_consistency,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * # Exercises hashed index bug in SERVER-102302. Once that fix is backported, this fcv
 * # requirement can be removed.
 * requires_fcv_82,
 * ]
 */
import {getDifferentlyShapedQueries} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getAggPipelineModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {
    runDeoptimized,
    testProperty
} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";

let numRuns = 150;
if (isSlowBuild(db)) {
    numRuns = 5;
    jsTestLog('Trying less examples because debug is on, opt is off, or a sanitizer is enabled.');
}
const numQueriesPerRun = 10;

const controlColl = db.run_all_plans_control;
const experimentColl = db.run_all_plans_experiment;

function runHintedAgg(query, index) {
    try {
        return {docs: experimentColl.aggregate(query, {hint: index.name}).toArray()};
    } catch (e) {
        return {err: e.code};
    }
}

function hintedQueryHasSameResultsAsControlCollScan(getQuery, testHelpers) {
    const indexes = experimentColl.getIndexes();
    const queries = getDifferentlyShapedQueries(getQuery, testHelpers);

    // Compute the control results all at once.
    const resultMap = runDeoptimized(controlColl, queries);

    for (let i = 0; i < queries.length; i++) {
        const query = queries[i];
        const controlResults = resultMap[i];
        for (const index of indexes) {
            const res = runHintedAgg(query, index);
            assert(res.err || res.docs);
            if (res.err && res.err !== ErrorCodes.NoQueryExecutionPlans) {
                return {
                    passed: false,
                    message: 'Hinting index led to unexpected error.',
                    query,
                    error: res.err,
                    index
                };
            } else if (res.docs && !testHelpers.comp(controlResults, res.docs)) {
                return {
                    passed: false,
                    message:
                        'Query results from hinted experiment collection did not match plain collection using collscan.',
                    query,
                    index,
                    explain: experimentColl.explain().aggregate(query, {hint: index.name}),
                    controlResults,
                    docsInCollection: controlColl.find().toArray(),
                    experimentalResults: res.docs
                };
            }
        }
    }
    return {passed: true};
}

const aggModel = getAggPipelineModel();

// Test with a regular collection.
testProperty(
    hintedQueryHasSameResultsAsControlCollScan,
    {controlColl, experimentColl},
    // Hinting a partial index can return incorrect results due to SERVER-26413.
    // TODO SERVER-26413 re-enable partial index coverage.
    makeWorkloadModel(
        {collModel: getCollectionModel({allowPartialIndexes: false}), aggModel, numQueriesPerRun}),
    numRuns);

// TODO SERVER-103381 re-enable timeseries PBT testing.
// Test with a TS collection.
// {
//     // TODO SERVER-83072 re-enable $group in this test, by removing the filter below.
//     const tsAggModel = aggModel.filter(query => {
//         for (const stage of query) {
//             if (Object.keys(stage).includes('$group')) {
//                 return false;
//             }
//         }
//         return true;
//     });
//     testProperty(
//         hintedQueryHasSameResultsAsControlCollScan,
//         {controlColl, experimentColl},
//         makeWorkloadModel(
//             {collModel: getCollectionModel({isTS: true}), aggModel: tsAggModel,
//             numQueriesPerRun}),
//         numRuns);
// }
