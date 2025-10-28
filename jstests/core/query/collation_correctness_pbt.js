/**
 * A property-based test that asserts the correctness of collated queries.
 *
 * @tags: [
 * query_intensive_pbt,
 * # Runs queries that may return many results, requiring getmores.
 * requires_getmore,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * ]
 */

import {getQueryAndOptionsModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {createCorrectnessProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {getMatchArb} from "jstests/libs/property_test_helpers/models/match_models.js";
import {
    simpleProjectArb,
    addFieldsConstArb,
    computedProjectArb,
    addFieldsVarArb,
    getSortArb,
} from "jstests/libs/property_test_helpers/models/query_models.js";

if (isSlowBuild(db)) {
    jsTestLog("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 40;
const numQueriesPerRun = 40;

const controlColl = db.collation_pbt_control;
const experimentColl = db.collation_pbt_experiment;

const correctnessProperty = createCorrectnessProperty(controlColl, experimentColl);

// Note that we exclude $group here on purpose. With collation, different strings will compare as
// equal, but the output of the $group stage depends on which one is seen first. This is not
// guaranteed to be the same across the control/experiment collections.
const allowedStages = [
    simpleProjectArb,
    getMatchArb(),
    addFieldsConstArb,
    computedProjectArb,
    addFieldsVarArb,
    getSortArb(),
];
const aggModel = getQueryAndOptionsModel({allowCollation: true, allowedStages: allowedStages});

testProperty(
    correctnessProperty,
    {controlColl, experimentColl},
    makeWorkloadModel({collModel: getCollectionModel(), aggModel, numQueriesPerRun}),
    numRuns,
);
