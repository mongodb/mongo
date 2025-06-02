/**
 * A property-based test that runs queries with random query knobs set and asserts the correctness
 * compared to a collection scan with no knobs set.
 *
 * @tags: [
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * requires_timeseries,
 * # TODO SERVER-104420 consider removing the below tag
 * assumes_no_implicit_collection_creation_on_get_collection,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Change in read concern can slow down queries enough to hit a timeout.
 * assumes_read_concern_unchanged,
 * does_not_support_causal_consistency,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * # Some query knobs may not exist on older versions.
 * multiversion_incompatible
 * ]
 */
import {getDifferentlyShapedQueries} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {queryKnobsModel} from "jstests/libs/property_test_helpers/models/query_knob_models.js";
import {getAggPipelineModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {
    runDeoptimized,
    testProperty
} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

if (isSlowBuild(db)) {
    jsTestLog("Exiting early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 30;
const numQueriesPerRun = 50;

const controlColl = db.query_knob_correctness_pbt_control;
const experimentColl = db.query_knob_correctness_pbt_experiment;

/*
 * Runs the given function with the query knobs set, then sets the query knobs back to their
 * original state.
 * It's important that each run of the property is independent from one another, so we'll always
 * reset the knobs to their original state even if the function throws an exception.
 */
function runWithKnobs(knobToVal, fn) {
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
    assert.commandWorked(db.adminCommand({setParameter: 1, ...knobToVal}));

    // With the finally block, we'll always revert the parameters back to their original settings,
    // even if an exception is thrown.
    try {
        return fn();
    } finally {
        // Reset to the original settings.
        // TODO SERVER-104420 consider using `runCommandOnAllShards` to send the command to every
        // shard.
        assert.commandWorked(db.adminCommand({setParameter: 1, ...priorSettings}));
    }
}

function queriesWithKnobsSetAreSameAsControlCollScan(getQuery, testHelpers, knobToVal) {
    const queries = getDifferentlyShapedQueries(getQuery, testHelpers);

    // Compute the control results all at once.
    const resultMap = runDeoptimized(controlColl, queries);

    return runWithKnobs(knobToVal, () => {
        for (let i = 0; i < queries.length; i++) {
            const query = queries[i];
            const controlResults = resultMap[i];
            const experimentResults = experimentColl.aggregate(query).toArray();
            if (!testHelpers.comp(controlResults, experimentResults)) {
                return {
                    passed: false,
                    message:
                        'A query with different knobs set has returned incorrect results compared to a collection scan query with no knobs set.',
                    query,
                    explain: experimentColl.explain().aggregate(query),
                    controlResults,
                    experimentResults,
                    knobToVal
                };
            }
        }
        return {passed: true};
    });
}

function getWorkloadModel(isTS, aggModel) {
    return fc
        .record({
            collSpec: getCollectionModel({isTS}),
            queries: fc.array(aggModel, {minLength: numQueriesPerRun, maxLength: numQueriesPerRun}),
            knobToVal: queryKnobsModel
        })
        .map(({collSpec, queries, knobToVal}) => {
            return {collSpec, queries, extraParams: [knobToVal]};
        });
}

// Test with a regular collection.
testProperty(queriesWithKnobsSetAreSameAsControlCollScan,
             {controlColl, experimentColl},
             getWorkloadModel(false /* isTS */, getAggPipelineModel()),
             numRuns);

// TODO SERVER-103381 re-enable timeseries PBT testing.
// Test with a TS collection.
// TODO SERVER-83072 re-enable $group in this test, by removing the filter below.
// const tsAggModel = getAggPipelineModel().filter(query => {
//     for (const stage of query) {
//         if (Object.keys(stage).includes('$group')) {
//             return false;
//         }
//     }
//     return true;
// });
// testProperty(queriesWithKnobsSetAreSameAsControlCollScan,
//              {controlColl, experimentColl},
//              getWorkloadModel(true /* isTS */, tsAggModel),
//              numRuns);
