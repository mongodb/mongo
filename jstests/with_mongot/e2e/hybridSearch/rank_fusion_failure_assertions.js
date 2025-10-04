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
 *   requires_fcv_82
 * ]
 */

const collName = jsTestName();
const coll = db[collName];

function runPipeline(pipeline) {
    return db.runCommand({aggregate: collName, pipeline, cursor: {}});
}

// Check that unranked pipeline is invalid.
assert.commandFailedWithCode(runPipeline([{$rankFusion: {input: {pipelines: {searchone: [{$limit: 5}]}}}}]), 9191100);

// Check that non-selection pipeline is invalid
assert.commandFailedWithCode(
    runPipeline([
        {
            $rankFusion: {input: {pipelines: {searchone: [{$sort: {_id: 1}}, {$project: {score1: 1}}]}}},
        },
    ]),
    9191103,
);

assert.commandFailedWithCode(
    runPipeline([
        {
            $rankFusion: {
                input: {
                    pipelines: {nested: [{$rankFusion: {input: {pipelines: {simple: [{$sort: {_id: 1}}]}}}}]},
                },
            },
        },
    ]),
    10473002,
);

assert.commandFailedWithCode(
    runPipeline([
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        nested: [
                            {
                                $scoreFusion: {
                                    input: {
                                        pipelines: {simple: [{$score: {score: "$score_50"}}]},
                                        normalization: "sigmoid",
                                    },
                                },
                            },
                        ],
                    },
                },
            },
        },
    ]),
    10473002,
);

assert.commandFailedWithCode(
    runPipeline([
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        nested: [{$limit: 10}, {$rankFusion: {input: {pipelines: {simple: [{$sort: {_id: 1}}]}}}}],
                    },
                },
            },
        },
    ]),
    10170100,
);

// Check that LPP validation catches that $rankFusion is not the first stage. This test may help
// expose discrepancies across sharding topologies.
assert.commandFailedWithCode(
    runPipeline([{$limit: 10}, {$rankFusion: {input: {pipelines: {nested: [{$sort: {_id: 1}}]}}}}]),
    10170100,
);

// Check that $score is not an allowed stage in $rankFusion.
assert.commandFailedWithCode(
    runPipeline([
        {
            $rankFusion: {
                input: {pipelines: {scoreInputPipeline: [{$score: {score: "$_id"}}, {$sort: {_id: 1}}]}},
            },
        },
    ]),
    10614800,
);
assert.commandFailedWithCode(
    runPipeline([
        {
            $rankFusion: {
                input: {pipelines: {scoreInputPipeline: [{$sort: {_id: 1}}, {$score: {score: "$_id"}}]}},
            },
        },
    ]),
    10614800,
);
assert.commandFailedWithCode(
    runPipeline([
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        scoreInputPipeline1: [{$score: {score: "$_id"}}, {$sort: {_id: 1}}],
                        scoreInputPipeline2: [{$sort: {_id: 1}}],
                    },
                },
            },
        },
    ]),
    10614800,
);
assert.commandFailedWithCode(
    runPipeline([
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        scoreInputPipeline1: [{$sort: {_id: 1}}],
                        scoreInputPipeline2: [{$score: {score: "$_id"}}, {$sort: {_id: 1}}],
                    },
                },
            },
        },
    ]),
    10614800,
);
