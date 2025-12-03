/**
 * A property-based test that runs queries with random query knobs set and asserts the correctness
 * compared to a collection scan with no knobs set.
 *
 * @tags: [
 * query_intensive_pbt,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * config_shard_incompatible,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * # Some query knobs may not exist on older versions.
 * multiversion_incompatible,
 * ]
 */
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {queryKnobsModel} from "jstests/libs/property_test_helpers/models/query_knob_models.js";
import {getQueryAndOptionsModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {createQueriesWithKnobsSetAreSameAsControlCollScanProperty} from "jstests/libs/property_test_helpers/common_properties.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Exiting early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 30;
const numQueriesPerRun = 50;

const controlColl = db.query_knob_correctness_pbt_control;
const experimentColl = db.query_knob_correctness_pbt_experiment;

function getWorkloadModel() {
    return fc
        .record({
            collSpec: getCollectionModel(),
            queries: fc.array(getQueryAndOptionsModel(), {minLength: 1, maxLength: numQueriesPerRun}),
            knobToVal: queryKnobsModel,
        })
        .map(({collSpec, queries, knobToVal}) => {
            return {collSpec, queries, extraParams: {knobToVal}};
        });
}

const knobCorrectnessProperty = createQueriesWithKnobsSetAreSameAsControlCollScanProperty(controlColl, experimentColl);

// Test with a regular collection.
testProperty(knobCorrectnessProperty, {controlColl, experimentColl}, getWorkloadModel(), numRuns);
