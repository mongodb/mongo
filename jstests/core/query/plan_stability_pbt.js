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

import {getDifferentlyShapedQueries} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getAggPipelineModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {
    planStabilityCounterexamples
} from "jstests/libs/property_test_helpers/pbt_resolved_bugs.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {getRejectedPlans, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

if (isSlowBuild(db)) {
    jsTestLog("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 40;
const numQueriesPerRun = 50;

const experimentColl = db.plan_stability_pbt;

// Checks if the winning plan is the same between explains, and the same rejected plans exist.
function sameWinningAndRejectedPlans(explain1, explain2) {
    // When CBR non-deterministic sampling is enabled, this function should remove the cost and CE
    // estimates from the plans, and assert that the same set of candidate plans are considered (no
    // assertion on the winning plan).
    const cmp = friendlyEqual;
    return cmp(getWinningPlanFromExplain(explain1), getWinningPlanFromExplain(explain2)) &&
        cmp(getRejectedPlans(explain1), getRejectedPlans(explain2));
}

function correctnessProperty(getQuery, testHelpers) {
    const queries = getDifferentlyShapedQueries(getQuery, testHelpers);

    for (const query of queries) {
        // Run explain on the query once to get the initial winning plan. Then we run explain ten
        // more times to assert that the winning plan is the same each time.
        const initialExplain = experimentColl.explain().aggregate(query);

        for (let i = 0; i < 10; i++) {
            const newExplain = experimentColl.explain().aggregate(query);
            if (!sameWinningAndRejectedPlans(initialExplain, newExplain)) {
                return {
                    passed: false,
                    message:
                        'A query was found to have unstable plan selection across runs with the same documents and indexes.',
                    initialExplain,
                    newExplain
                };
            }
        }
    }
    return {passed: true};
};

// TODO SERVER-106983, re-enable $match once planner is deterministic.
const aggModel = getAggPipelineModel().filter(q => !JSON.stringify(q).includes('$match'));

testProperty(
    correctnessProperty,
    {experimentColl},
    makeWorkloadModel({collModel: getCollectionModel(), aggModel, numQueriesPerRun}),
    numRuns,
    // TODO SERVER-106983 re-enable counterexample runs.
    //  planStabilityCounterexamples
);
