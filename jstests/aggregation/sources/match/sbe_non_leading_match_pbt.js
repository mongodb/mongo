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
import {sbePushdownEligibleAggModel} from "jstests/libs/property_test_helpers/common_models.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 10;
const numQueriesPerRun = 10;
const jsTestLogExplain = false;

const controlName = "match_pbt_control";
const experimentName = "match_pbt_experiment";

const controlColl = db[controlName];
const experimentColl = db[experimentName];

const correctnessProperty = createCorrectnessProperty(
    controlColl,
    experimentColl,
    undefined /*statsCollectorFn*/,
    jsTestLogExplain,
);

// The inner side of the lookup may be out-of-order between control and experiment. There are
// $unwind's sprinkled in as a workaround. This blocks SBE pushdown for some suffix of the
// pipeline.
// TODO SERVER-115463 use a new comparator with kSortArrays.
testProperty(
    correctnessProperty,
    {controlColl, experimentColl},
    // Control collection is foreign side.
    makeWorkloadModel({
        collModel: getCollectionModel(),
        aggModel: sbePushdownEligibleAggModel(controlName),
        numQueriesPerRun,
    }),
    numRuns,
);

testProperty(
    correctnessProperty,
    {controlColl, experimentColl},
    // Experiment collection is foreign side.
    makeWorkloadModel({
        collModel: getCollectionModel(),
        aggModel: sbePushdownEligibleAggModel(experimentName),
        numQueriesPerRun,
    }),
    numRuns,
);
