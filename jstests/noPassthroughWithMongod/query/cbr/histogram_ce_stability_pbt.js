/**
 * A property-based test that asserts stability of cardinality and cost estimates for
 * histogram-based plan ranking.
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

// Use less runs so we run `analyze` less.
const numRuns = 30;
const numQueriesPerRun = 60;

const experimentColl = db[jsTestName()];
assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));
const planStabilityFn = createPlanStabilityProperty(experimentColl, true /* assertCeExists */);

function histogramPlanStabilityProperty(getQuery, testHelpers, {numberBuckets}) {
    // Run analyze on all possible fields the queries could use. This includes all top-level
    // fields, in addition to the dotted paths used, "m1" and "m2".
    const prefixes = ['a', 'b', 't', 'm', '_id', 'array'];
    const suffixes = ['', '.m1', '.m2'];
    const allFields = prefixes.flatMap(p => suffixes.map(s => p + s));
    for (const analyzeKey of allFields) {
        assert.commandWorked(experimentColl.runCommand(
            {analyze: experimentColl.getName(), key: analyzeKey, numberBuckets}));
    }

    return planStabilityFn(getQuery, testHelpers);
}

testProperty(histogramPlanStabilityProperty,
             {experimentColl},
             createStabilityWorkload(numQueriesPerRun),
             numRuns);
