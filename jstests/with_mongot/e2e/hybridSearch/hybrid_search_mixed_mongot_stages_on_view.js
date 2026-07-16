/**
 * Tests $rankFusion/$scoreFusion on a view mixing a $search input pipeline with a $vectorSearch
 * one, with featureFlagExtensionsInsideHybridSearch enabled and disabled. In the
 * with_mongot_extension_* suites $vectorSearch is an extension while $search stays legacy: flag
 * on requires the extension stage to keep its spec (notably `index`) through view resolution
 * (SERVER-131307); flag off must still succeed via the IFR kickback. In the mongot_e2e_* suites
 * both stages run as legacy stages.
 *
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_90, assumes_stable_shard_list]
 */

import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {
    createHybridSearchPipeline,
    runHybridSearchViewTest,
    searchPipelineBar,
    vectorSearchPipelineV,
} from "jstests/with_mongot/e2e_lib/hybrid_search_on_view.js";

function createRankFusionPipeline(inputPipelines, viewPipeline = null) {
    const rankFusionStage = {$rankFusion: {input: {pipelines: {}}}};
    return createHybridSearchPipeline(inputPipelines, viewPipeline, rankFusionStage);
}

function createScoreFusionPipeline(inputPipelines, viewPipeline = null) {
    const scoreFusionStage = {
        $scoreFusion: {
            input: {pipelines: {}, normalization: "sigmoid"},
            combination: {method: "avg"},
        },
    };
    return createHybridSearchPipeline(
        inputPipelines,
        viewPipeline,
        scoreFusionStage,
        /**isRankFusion*/ false,
    );
}

const kFlagName = "featureFlagExtensionsInsideHybridSearch";
const kViewPipeline = [{$match: {"$expr": {$lt: ["$x", 10]}}}];

function makeTestCases(suffix) {
    const testCases = [];
    for (const [fusionKind, createStagePipelineFn] of [
        ["rank_fusion", createRankFusionPipeline],
        ["score_fusion", createScoreFusionPipeline],
    ]) {
        testCases.push({
            testName: `${fusionKind}_multi_search_${suffix}`,
            inputPipelines: {a: [searchPipelineBar], b: [vectorSearchPipelineV]},
            createStagePipelineFn,
        });
        testCases.push({
            testName: `${fusionKind}_swapped_multi_search_${suffix}`,
            inputPipelines: {a: [vectorSearchPipelineV], b: [searchPipelineBar]},
            createStagePipelineFn,
        });
    }
    return testCases;
}

function runSubcasesWithFlag(flagValue) {
    const testCases = makeTestCases(flagValue ? "flag_on" : "flag_off");
    runWithParamsAllNonConfigNodes(db, {[kFlagName]: flagValue}, () => {
        try {
            for (const {testName, inputPipelines, createStagePipelineFn} of testCases) {
                jsTestLog(`Running subcase: ${testName}`);
                runHybridSearchViewTest(
                    testName,
                    inputPipelines,
                    kViewPipeline,
                    /*checkCorrectness=**/ true,
                    /*isMongotPipeline=**/ true,
                    createStagePipelineFn,
                );
            }
        } finally {
            // Drop the views this test created so reruns against a shared fixture don't collide.
            for (const {testName} of testCases) {
                db.runCommand({drop: jsTestName() + "_" + testName + "_view"});
            }
        }
    });
}

runSubcasesWithFlag(true);
runSubcasesWithFlag(false);
