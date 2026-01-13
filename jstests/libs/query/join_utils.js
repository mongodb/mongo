/**
 * Utility functions for join optimization tests.
 */
import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

// Runs the given test case with join optimization enabled and disabled, verifies that the results
// match expectedResults with UNORDERED comparison, and checks whether the join optimizer was used as expected.
export function runTestWithUnorderedComparison({
    description,
    coll,
    pipeline,
    expectedResults,
    expectedUsedJoinOptimization,
    additionalJoinParams = {},
}) {
    print(`Running test: ${description} with additional join params ${additionalJoinParams}`);

    const originalResults = runWithParamsAllNonConfigNodes(db, {internalEnableJoinOptimization: false}, () => {
        return coll.aggregate(pipeline).toArray();
    });
    assert.sameMembers(originalResults, expectedResults);

    const joinParams = {internalEnableJoinOptimization: true, ...additionalJoinParams};
    const joinOptResults = runWithParamsAllNonConfigNodes(db, joinParams, () => {
        return coll.aggregate(pipeline).toArray();
    });
    assert.sameMembers(joinOptResults, expectedResults);

    const explain = runWithParamsAllNonConfigNodes(db, joinParams, () => {
        return coll.explain().aggregate(pipeline);
    });
    print(`Explain: ${tojson(explain)}`);
    const winningPlan = getQueryPlanner(explain).winningPlan;

    const usedJoinOptimization = winningPlan.hasOwnProperty("usedJoinOptimization")
        ? winningPlan.usedJoinOptimization
        : false;
    assert.eq(expectedUsedJoinOptimization, usedJoinOptimization, winningPlan);
}
