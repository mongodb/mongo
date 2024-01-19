import {
    checkCascadesOptimizerEnabled,
    removeUUIDsFromExplain,
    runWithParams
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
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

function assertPipelineParameterization(cmd, isEligible) {
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
        assertPipelineParameterization([], false);

        // Agg pipeline with single $match stage is eligible.
        assertPipelineParameterization([{$match: {a: 2, b: 3}}], true);

        // Agg pipeline with $match as first stage, $project as second stage is eligible.
        assertPipelineParameterization([{$match: {a: {$gte: 2}}}, {$project: {a: 1, _id: 0}}],
                                       true);

        // Agg pipeline with $match as first stage but a second stage that's not $project is
        // ineligible.
        assertPipelineParameterization([{$match: {a: {$in: [2, 3]}}}, {$group: {_id: "$a"}}],
                                       false);

        // Agg pipeline with $match as first stage, $project as second stage, and additional stages
        // is ineligible.
        assertPipelineParameterization(
            [{$match: {a: {$gte: 2}}}, {$project: {a: 1, _id: 0}}, {$group: {_id: "$a"}}], false);
    });
