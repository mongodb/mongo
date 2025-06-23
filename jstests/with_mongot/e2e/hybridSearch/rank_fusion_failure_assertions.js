/**
 * Duplicate JS tests for some of the edge cases found in
 * src/mongo/db/pipeline/document_source_rank_fusion_test.cpp
 *
 * These will be picked up/ran by the js fuzzer suite.
 *
 * @tags: [
 *   featureFlagRankFusionFull,
 *   # Needed for the nested $scoreFusion.
 *   featureFlagSearchHybridScoringFull,
 *   requires_fcv_81
 * ]
 */

const collName = "search_rank_fusion";
const coll = db[collName];

function runPipeline(pipeline) {
    return db.runCommand({aggregate: collName, pipeline, cursor: {}});
}

// Check that unranked pipeline is invalid.
assert.commandFailedWithCode(
    runPipeline([{$rankFusion: {input: {pipelines: {searchone: [{$limit: 5}]}}}}]), 9191100);

// Check that non-selection pipeline is invalid
assert.commandFailedWithCode(
    runPipeline([{
        $rankFusion: {input: {pipelines: {searchone: [{$sort: {_id: 1}}, {$project: {score1: 1}}]}}}
    }]),
    9191103);

assert.commandFailedWithCode(
    runPipeline([{
        $rankFusion: {
            input: {
                pipelines:
                    {nested: [{$rankFusion: {input: {pipelines: {simple: [{$sort: {_id: 1}}]}}}}]}
            }
        }
    }]),
    10473002);

assert.commandFailedWithCode(
    runPipeline([{
        $rankFusion: {
            input: {
                pipelines: {
                    nested: [{
                        $scoreFusion: {
                            input: {
                                pipelines: {simple: [{$score: {score: "$score_50"}}]},
                                normalization: "sigmoid"
                            },
                        }
                    }]
                }
            }
        }
    }]),
    10473002);

assert.commandFailedWithCode(
    runPipeline([{
        $rankFusion: {
            input: {
                pipelines: {
                    nested: [
                        {$limit: 10},
                        {$rankFusion: {input: {pipelines: {simple: [{$sort: {_id: 1}}]}}}}
                    ]
                }
            }
        }
    }]),
    10170100);
