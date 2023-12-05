import {
    checkCascadesOptimizerEnabled,
    checkPlanCacheParameterization,
    removeUUIDsFromExplain,
    runWithParams
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

// TODO SERVER-82185: Remove this once M2-eligibility checker + E2E parameterization implemented
if (!checkPlanCacheParameterization(db)) {
    jsTestLog("Skipping test because E2E plan cache parameterization not yet implemented");
    quit();
}

// Checks that eligible pipelines undergo auto-parameterization.
// The translated and optimized ABT should include the getParam FunctionCall node.
// An eligible pipeline must:
// (1) Have its first stage be a $match stage
// (2) If it has additional stages, the second stage must be a $project stage
function getParamExists(explainOutput) {
    return explainOutput.includes("getParam");
}

function checkPipelineEligibility(cmd, isEligible) {
    let res = runWithParams(
        [
            {key: 'internalCascadesOptimizerExplainVersion', value: "v2"},
            {key: "internalCascadesOptimizerUseDescriptiveVarNames", value: true}
        ],
        () => t.explain("executionStats").aggregate(cmd));
    let explainStr = removeUUIDsFromExplain(db, res);
    assert(isEligible ? getParamExists(explainStr) : !getParamExists(explainStr));
}

const t = db.cqf_parameterized_agg_test;
t.drop();

assert.commandWorked(t.insert({_id: 3, a: 1}));

runWithParams(
    [
        // Disable fast-path since it bypasses parameterization and optimization.
        {key: "internalCascadesOptimizerDisableFastPath", value: true},
    ],
    () => {
        // Empty agg pipeline is ineligible for the optimizer plan cache.
        checkPipelineEligibility([], false);

        // Agg pipeline with single $match stage is eligible.
        checkPipelineEligibility([{$match: {a: 2, b: 3}}], true);

        // Agg pipeline with $match as first stage, $project as second stage is eligible.
        checkPipelineEligibility([{$match: {a: {$gte: 2}}}, {$project: {a: 1, _id: 0}}], true);

        // Agg pipeline with $match as first stage but a second stage that's not $project is
        // ineligible.
        checkPipelineEligibility([{$match: {a: {$in: [2, 3]}}}, {$group: {_id: "$a"}}], false);
    });
