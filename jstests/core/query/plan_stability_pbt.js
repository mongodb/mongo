/**
 * A property-based test that asserts the same candidate plans are considered when running a query
 * multiple times.
 * For the multiplanner and deterministic cost-based ranking modes, we can assert the same winning
 * plan is chosen. For non-deterministic sampling, we'll have to skip this assertion.
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

import {createPlanStabilityProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getQueryAndOptionsModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {planStabilityCounterexamples} from "jstests/libs/property_test_helpers/pbt_resolved_bugs.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";

if (isSlowBuild(db)) {
    jsTestLog("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 40;
const numQueriesPerRun = 50;

const experimentColl = db.plan_stability_pbt;

// TODO SERVER-106983, re-enable $match once planner is deterministic.
const aggModel = getQueryAndOptionsModel().filter((q) => !JSON.stringify(q).includes("$match"));

testProperty(
    createPlanStabilityProperty(experimentColl),
    {experimentColl},
    makeWorkloadModel({collModel: getCollectionModel(), aggModel, numQueriesPerRun}),
    numRuns,
    // TODO SERVER-106983 re-enable counterexample runs.
    //  planStabilityCounterexamples
);
