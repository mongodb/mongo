/**
 * A property-based test that runs queries with "internalQueryPermitMatchSwappingForComplexRenames"
 * enabled and asserts the correctness by comparing results with the knob disabled.
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
 * multiversion_incompatible
 * ]
 */

import {isSlowBuild} from "jstests/libs/aggregation_pipeline_utils.js";
import {
    createQueriesWithKnobsSetAreSameAsControlCollScanProperty
} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getDocsModel} from "jstests/libs/property_test_helpers/models/document_models.js";
import {
    getNestedDocModelNoArray
} from "jstests/libs/property_test_helpers/models/document_models.js";
import {groupArb} from "jstests/libs/property_test_helpers/models/group_models.js";
import {getMatchArb} from "jstests/libs/property_test_helpers/models/match_models.js";
import {
    addFieldsVarArb,
    computedProjectArb
} from "jstests/libs/property_test_helpers/models/query_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

if (isSlowBuild(db)) {
    jsTestLog("Exiting early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 30;
const numQueriesPerRun = 50;

const controlColl = db.query_knob_correctness_pbt_control;
const experimentColl = db.query_knob_correctness_pbt_experiment;

const knobCorrectnessProperty =
    createQueriesWithKnobsSetAreSameAsControlCollScanProperty(controlColl, experimentColl);

// The property only holds when the docs don't contain arrays and pipelines don't generate nested
// arrays.
function getWorkloadModelForComplexRenameMatchSwap() {
    // Aggregations are 'renaming' stage followed by a match stage.
    const renamingArb = fc.oneof(computedProjectArb, addFieldsVarArb, groupArb);
    const aggModel = fc.tuple(renamingArb, getMatchArb());

    // This document model generates very nested objects that do not contain any arrays.
    const docModel = getNestedDocModelNoArray();
    const docsModel = getDocsModel({docModel});

    // Because we don't have as much control over types here, we need to remove the indexes because
    // otherwise they are likely to fail to build. Comparing results with collection scans only is
    // sufficient for detecting an incorrect rewrite here.
    const indexesModel = fc.constant([]);

    return fc
        .record({
            collSpec: getCollectionModel({docsModel, indexesModel}),
            queries: fc.array(aggModel, {minLength: 1, maxLength: numQueriesPerRun}),
            knobToVal: fc.constant({internalQueryPermitMatchSwappingForComplexRenames: true}),
        })
        .map(({collSpec, queries, knobToVal}) => {
            return {collSpec, queries, extraParams: {knobToVal}};
        });
}
testProperty(
    knobCorrectnessProperty,
    {controlColl, experimentColl},
    getWorkloadModelForComplexRenameMatchSwap(),
    numRuns,
);
