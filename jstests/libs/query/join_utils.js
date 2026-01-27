/**
 * Utility functions for join optimization tests.
 */
import {getQueryPlanner, getAllPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
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

/**
 * Ensures the winning query plan used a hash join.
 */
export function usedHashJoinEmbedding(explain) {
    const stages = getAllPlanStages(getWinningPlanFromExplain(explain)).map((stage) => stage.stage);
    return stages.includes("HASH_JOIN_EMBEDDING");
}
/**
 * Ensures the winning query plan used a indexed nested loop join.
 */
export function usedIndexedNestLoopJoinEmbedding(explain) {
    const stages = getAllPlanStages(getWinningPlanFromExplain(explain)).map((stage) => stage.stage);
    return stages.includes("INDEXED_NESTED_LOOP_JOIN_EMBEDDING");
}
/**
 * Ensures the winning query plan used a nested loop join.
 */
export function usedNestedLoopJoinEmbedding(explain) {
    const stages = getAllPlanStages(getWinningPlanFromExplain(explain)).map((stage) => stage.stage);
    return stages.includes("NESTED_LOOP_JOIN_EMBEDDING");
}

/**
 * Returns a boolean that indicates if the explain showed that join optimization was used.
 */
export function joinOptUsed(explain) {
    const winningPlanStats = getQueryPlanner(explain).winningPlan;

    if (winningPlanStats.usedJoinOptimization === false) {
        return false;
    }

    return (
        winningPlanStats.usedJoinOptimization &&
        (usedHashJoinEmbedding(explain) ||
            usedIndexedNestLoopJoinEmbedding(explain) ||
            usedNestedLoopJoinEmbedding(explain))
    );
}
