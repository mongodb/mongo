/**
 * Tests multiple combinations of $scoreFusion syntax. Note that more in-depth testing for
 * $scoreFusion and $score exists inside of jstests/with_mongot/e2e/hybridSearch. This test exists
 * in order to get picked up by the fuzzer/query shape hash stability tests (tests must exist inside
 * of the jstests/aggregation directory).
 * @tags: [ featureFlagSearchHybridScoringFull, do_not_wrap_aggregations_in_facets, requires_fcv_81
 * ]
 */

import {crossProduct} from "jstests/libs/query/query_settings_index_hints_tests.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
const nDocs = 10;

for (let i = 1; i <= nDocs; i++) {
    bulk.insert({i, single: i});
}
assert.commandWorked(bulk.execute());

const normalizationOptions = ["none", "sigmoid", "minMaxScaler"];
const weights = [0.2, 1];
const combinationMethods = ["avg", "expression"];

const combinationExpressionForSinglePipeline = {
    $add: ["$$pipeline1"]
};

const combinationExpressionForMultiPipeline = {
    $add: ["$$pipeline1", "$$pipeline2"]
};

const scoreSpecs = ["$single", {$add: ["$single", 5]}];

function runSingleInputPipelineScoreFusion(scoreFusionNormalization,
                                           inputPipelineWeight,
                                           scoreNormalization,
                                           combinationMethod,
                                           scoreSpec) {
    let combinationSpec = {method: combinationMethod};

    if (combinationMethod === "expression") {
        combinationSpec["expression"] = combinationExpressionForSinglePipeline;
    } else if (combinationMethod === "avg") {
        combinationSpec["weights"] = {pipeline1: inputPipelineWeight};
    }

    let query = {
        $scoreFusion: {
            input: {
                pipelines:
                    {pipeline1: [{$score: {score: scoreSpec, normalization: scoreNormalization}}]},
                normalization: scoreFusionNormalization
            },
            combination: combinationSpec
        },
    };

    coll.aggregate([query]).toArray();
}

// Test syntax options for a single input pipeline.
for (const [scoreFusionNormalization,
            firstPipelineWeight,
            scoreNormalization,
            combinationMethod,
            scoreSpec] of crossProduct(normalizationOptions,
                                       weights,
                                       normalizationOptions,
                                       combinationMethods,
                                       scoreSpecs)) {
    runSingleInputPipelineScoreFusion(scoreFusionNormalization,
                                      firstPipelineWeight,
                                      scoreNormalization,
                                      combinationMethod,
                                      scoreSpec);
}

// Test syntax options for multiple input pipelines.
function runMultipleInputPipelineScoreFusion(scoreFusionNormalization,
                                             firstPipelineWeight,
                                             secondPipelineWeight,
                                             firstScoreNormalization,
                                             secondScoreNormalization,
                                             firstScoreSpec,
                                             secondScoreSpec,
                                             combinationMethod) {
    let combinationSpec = {method: combinationMethod};

    if (combinationMethod === "expression") {
        combinationSpec["expression"] = combinationExpressionForMultiPipeline;
    } else if (combinationMethod === "avg") {
        combinationSpec["weights"] = {
            pipeline1: firstPipelineWeight,
            pipeline2: secondPipelineWeight
        };
    }

    let query = {
        $scoreFusion: {
            input: {
                pipelines: {
                    pipeline1:
                        [{$score: {score: firstScoreSpec, normalization: firstScoreNormalization}}],
                    pipeline2: [
                        {$match: {single: {$gt: 5}}},
                        {$score: {score: secondScoreSpec, normalization: secondScoreNormalization}}
                    ]
                },
                normalization: scoreFusionNormalization
            },
            combination: combinationSpec
        },
    };

    coll.aggregate([query]).toArray();
}

for (const [scoreFusionNormalization,
            firstPipelineWeight,
            secondPipelineWeight,
            firstScoreNormalization,
            secondScoreNormalization,
            firstScoreSpec,
            secondScoreSpec,
            combinationMethod] of crossProduct(normalizationOptions,
                                               weights,
                                               weights,
                                               normalizationOptions,
                                               normalizationOptions,
                                               scoreSpecs,
                                               scoreSpecs,
                                               combinationMethods)) {
    runMultipleInputPipelineScoreFusion(scoreFusionNormalization,
                                        firstPipelineWeight,
                                        secondPipelineWeight,
                                        firstScoreNormalization,
                                        secondScoreNormalization,
                                        firstScoreSpec,
                                        secondScoreSpec,
                                        combinationMethod);
}
