/**
 * A property-based test that runs random queries with indexes (and the plan cache enabled) and
 * compares the results to the same queries, deoptimized.
 *
 * @tags: [
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * requires_timeseries,
 * assumes_no_implicit_collection_creation_on_get_collection,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Change in read concern can slow down queries enough to hit a timeout.
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

let numRuns = 200;
if (isSlowBuild(db)) {
    numRuns = 20;
    jsTestLog('Trying less examples because debug is on, opt is off, or a sanitizer is enabled.');
}

const controlColl = db.index_correctness_pbt_control;
const experimentColl = db.index_correctness_pbt_experiment;

function queryHasSameResultsAsControlCollScan(getQuery, testHelpers) {
    for (let queryIx = 0; queryIx < testHelpers.numQueryShapes; queryIx++) {
        const query = getQuery(queryIx, 0 /* paramIx */);
        if (query.length === 0) {
            continue;
        }

        const controlResults = runDeoptimizedQuery(controlColl, query);
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

const aggModel = getAggPipelineModel();

assert(controlColl.drop());
assert.commandWorked(controlColl.insert(defaultPbtDocuments()));

// Run the property with a regular collection.
assert(experimentColl.drop());
assert.commandWorked(experimentColl.insert(defaultPbtDocuments()));
testProperty(queryHasSameResultsAsControlCollScan,
             experimentColl,
             {aggModel, indexModel, numRuns, numQueriesPerRun: 20});

// TODO SERVER-101271 re-enable PBT testing for time-series
// Run the property with a TS collection.
// assert(experimentColl.drop());
// assert.commandWorked(db.createCollection(experimentColl.getName(), {
//     timeseries: {timeField: 't', metaField: 'm'},
// }));
// assert.commandWorked(experimentColl.insert(defaultPbtDocuments()));
// testProperty(queryHasSameResultsAsControlCollScan,
//              experimentColl,
//              {aggModel, indexModel: timeseriesIndexModel, numRuns, numQueriesPerRun: 20});
