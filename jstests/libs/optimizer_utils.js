load("jstests/libs/analyze_plan.js");

/*
 * Utility for checking if the query optimizer is enabled.
 */
function checkCascadesOptimizerEnabled(theDB) {
    const param = theDB.adminCommand({getParameter: 1, featureFlagCommonQueryFramework: 1});
    return param.hasOwnProperty("featureFlagCommonQueryFramework") &&
        param.featureFlagCommonQueryFramework.value;
}

/**
 * Given the result of an explain command, returns whether the bonsai optimizer was used.
 */
function usedBonsaiOptimizer(explain) {
    if (explain.hasOwnProperty("queryPlanner") &&
        !explain.queryPlanner.winningPlan.hasOwnProperty("optimizerPlan")) {
        // Find command explain which means new optimizer was not used.
        // TODO SERVER-62407 this assumption may no longer hold true if the translation to ABT
        // happens directly from a find command.
        return false;
    }

    const plannerOutput = getAggPlanStage(explain, "$cursor");
    if (plannerOutput != null) {
        return plannerOutput["$cursor"].queryPlanner.winningPlan.hasOwnProperty("optimizerPlan");
    } else {
        return explain.queryPlanner.winningPlan.hasOwnProperty("optimizerPlan");
    }
}