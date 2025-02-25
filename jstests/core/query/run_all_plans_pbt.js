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
 * ]
 */
import {
    indexModel,
    timeseriesIndexModel
} from "jstests/libs/property_test_helpers/models/index_models.js";
import {getAggPipelineModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {
    defaultPbtDocuments,
    runDeoptimizedQuery,
    testProperty
} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";

let numRuns = 150;
if (isSlowBuild(db)) {
    numRuns = 5;
    jsTestLog('Trying less examples because debug is on, opt is off, or a sanitizer is enabled.');
}

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

    for (let queryIx = 0; queryIx < testHelpers.numQueryShapes; queryIx++) {
        const query = getQuery(queryIx, 0 /* paramIx */);
        const controlResults = runDeoptimizedQuery(controlColl, query);
        for (const index of indexes) {
            const res = runHintedAgg(query, index);
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
                    experimentalResults: res.docs
                };
            }
        }
    }
    return {passed: true};
}

// TODO SERVER-99889 reenable testing for hashed indexes.
const indexModelNoHashed = indexModel.filter(index => {
    const isHashed = Object.values(index.def).includes('hashed');
    return !isHashed;
});

const aggModel = getAggPipelineModel();

assert(controlColl.drop());
assert.commandWorked(controlColl.insert(defaultPbtDocuments()));

// Run the property with a regular collection.
assert(experimentColl.drop());
assert.commandWorked(experimentColl.insert(defaultPbtDocuments()));
testProperty(hintedQueryHasSameResultsAsControlCollScan,
             experimentColl,
             {aggModel, indexModel: indexModelNoHashed, numRuns, numQueriesPerRun: 20});

// TODO SERVER-101271 re-enable PBT testing for time-series
// Run the property with a TS collection.
// assert(experimentColl.drop());
// assert.commandWorked(db.createCollection(experimentColl.getName(), {
//     timeseries: {timeField: 't', metaField: 'm'},
// }));
// assert.commandWorked(experimentColl.insert(defaultPbtDocuments()));
// testProperty(hintedQueryHasSameResultsAsControlCollScan,
//              experimentColl,
//              {aggModel, indexModel: timeseriesIndexModel, numRuns, numQueriesPerRun: 20});
