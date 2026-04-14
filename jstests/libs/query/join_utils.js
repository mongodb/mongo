/**
 * Utility functions for join optimization tests.
 */
import {getQueryPlanner, getAllPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

// Runs the given test case with join optimization enabled and disabled, verifies that the results
// match expectedResults with UNORDERED comparison, and checks whether the join optimizer was used as expected.
export function runTestWithUnorderedComparison({
    db,
    description,
    coll,
    pipeline,
    expectedResults,
    expectedUsedJoinOptimization,
    additionalJoinParams = {},
    expectedNumJoinStages = -1,
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

    const joinStages = getAllPlanStages(getWinningPlanFromExplain(explain)).filter(plannerStageIsJoinOptNode);
    if (expectedUsedJoinOptimization) {
        assert.gt(joinStages.length, 0, `Expected to find some join opt stages, found none: ` + tojson(explain));
        if (expectedNumJoinStages > 0) {
            assert.eq(
                joinStages.length,
                expectedNumJoinStages,
                `Expected ${expectedNumJoinStages} join opt stages: ` + tojson(explain),
            );
        }
    } else {
        assert.eq(joinStages.length, 0, `Expected no join opt stages: ` + tojson(explain));
    }
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
 * Returns an abbreviation for a join embedding stage name, or the original name
 * if it is not a join stage.
 */
export function joinStageAbbreviation(stageName) {
    switch (stageName) {
        case "HASH_JOIN_EMBEDDING":
            return "HJ";
        case "NESTED_LOOP_JOIN_EMBEDDING":
            return "NLJ";
        case "INDEXED_NESTED_LOOP_JOIN_EMBEDDING":
            return "INLJ";
        default:
            return stageName;
    }
}

export function plannerStageIsJoinOptNode(stageObj) {
    return (
        stageObj.stage.includes("NESTED_LOOP_JOIN_EMBEDDING") ||
        stageObj.stage.includes("INDEXED_NESTED_LOOP_JOIN_EMBEDDING") ||
        stageObj.stage.includes("HASH_JOIN_EMBEDDING")
    );
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

/**
 * Asserts that the join optimizer was used and every join node uses the expected method.
 */
export function assertAllJoinsUseMethod(explain, expectedMethod) {
    assert(joinOptUsed(explain), "Expected join optimization to be used: " + tojson(explain));

    const joinStages = getAllPlanStages(getWinningPlanFromExplain(explain)).filter(plannerStageIsJoinOptNode);
    assert.gt(joinStages.length, 0, "Expected at least one join stage: " + tojson(explain));

    for (const stage of joinStages) {
        assert.eq(
            joinStageAbbreviation(stage.stage),
            expectedMethod,
            `Expected all joins to be ${expectedMethod}, but found ${stage.stage}`,
        );
    }
}

/**
 * Restores join-opt parameters to state before test.
 */
export function joinTestWrapper(db, testFun) {
    const params = assert.commandWorked(
        db.adminCommand({
            getParameter: 1,
            internalEnableJoinOptimization: 1,
            internalJoinReorderMode: 1,
            internalJoinPlanTreeShape: 1,
            internalMaxNodesInJoinGraph: 1,
            internalMaxEdgesInJoinGraph: 1,
            internalMaxNumberNodesConsideredForImplicitEdges: 1,
            internalJoinPlanSamplingSize: 1,
            internalJoinEnumerateCollScanPlans: 1,
            internalMinAllPlansEnumerationSubsetLevel: 1,
            internalMaxAllPlansEnumerationSubsetLevel: 1,
            internalJoinOptimizationSamplingCEMethod: 1,
            internalJoinMethod: 1,
        }),
    );
    delete params.ok;
    delete params.operationTime;

    try {
        testFun();
    } finally {
        assert.commandWorked(db.adminCommand({setParameter: 1, ...params}));
    }
}
