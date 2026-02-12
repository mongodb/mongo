/**
 * A property-based test that asserts the correctness of queries with $group/$lookup followed by $match.
 *
 * @tags: [
 * query_intensive_pbt,
 * # Runs queries that may return many results, requiring getmores.
 * requires_getmore,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * ]
 */

import {createCorrectnessProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {checkSbeStatus, kSbeRestricted, kSbeDisabled} from "jstests/libs/query/sbe_util.js";
import {trySbeRestrictedPushdownEligibleAggModel} from "jstests/libs/property_test_helpers/common_models.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const sbeStatus = checkSbeStatus(db);
if (sbeStatus === kSbeDisabled) {
    jsTest.log.info("SBE is disabled, skipping test.");
    quit();
}

const numRuns = 6;
const numQueriesPerRun = 6;
const jsTestLogExplain = false;

const controlName = "match_pbt_control";
const experimentName = "match_pbt_experiment";

const controlColl = db[controlName];
const experimentColl = db[experimentName];

const correctnessProperty = createCorrectnessProperty(controlColl, experimentColl, {jsTestLogExplain});

// Lower the hash join threshold to increase the likelihood of hash joins being used.
runWithParamsAllNonConfigNodes(db, {internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin: 100}, () => {
    testProperty(
        correctnessProperty,
        {controlColl, experimentColl},
        // Control collection is foreign side.
        makeWorkloadModel({
            collModel: getCollectionModel(),
            aggModel: trySbeRestrictedPushdownEligibleAggModel(controlName),
            numQueriesPerRun,
        }),
        numRuns,
        undefined /*examples*/,
        true /*sortArrays*/,
    );

    testProperty(
        correctnessProperty,
        {controlColl, experimentColl},
        // Experiment collection is foreign side.
        makeWorkloadModel({
            collModel: getCollectionModel(),
            aggModel: trySbeRestrictedPushdownEligibleAggModel(experimentName),
            numQueriesPerRun,
        }),
        numRuns,
        undefined /*examples*/,
        true /*sortArrays*/,
    );
});
