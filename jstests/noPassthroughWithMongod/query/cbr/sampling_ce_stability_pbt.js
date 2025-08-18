/**
 * A property-based test that asserts stability of cardinality and cost estimates for
 * sampling-based plan ranking.
 *
 * @tags: [
 *    query_intensive_pbt,
 *    # Runs queries that may return many results, requiring getmores.
 *    requires_getmore,
 *    # This test runs commands that are not allowed with security token: setParameter.
 *    not_allowed_with_signed_security_token,
 *    # Any plan instability should be detectable from a standalone mongod. Sharded scenarios make
 *    # assertions more complicated because depending on which documents are on which shards, an
 *    # index may be multikey on one shard but not the other. Also different plans can be chosen
 *    # on different shards.
 *    assumes_standalone_mongod,
 *    # Candidate plans are not guaranteed to be stable across versions
 *    multiversion_incompatible
 * ]
 */

import {createStabilityWorkload} from "jstests/libs/property_test_helpers/common_models.js";
import {createPlanStabilityProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";

if (isSlowBuild(db)) {
    jsTestLog("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 40;
const numQueriesPerRun = 30;

const experimentColl = db[jsTestName()];
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: true}));
assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "samplingCE"}));
testProperty(createPlanStabilityProperty(experimentColl, true /* assertCeExists */),
             {experimentColl},
             createStabilityWorkload(numQueriesPerRun),
             numRuns);
