/**
 * Duplicate JS tests for some of the edge cases found in
 * src/mongo/db/pipeline/document_source_rank_fusion_test.cpp
 *
 * These will be picked up/ran by the js fuzzer suite.
 *
 * @tags: [
 *   featureFlagRankFusionFull,
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
                    nested: [
                        {$limit: 10},
                        {$rankFusion: {input: {pipelines: {simple: [{$sort: {_id: 1}}]}}}}
                    ]
                }
            }
        }
    }]),
    10170100);

// Check that LPP validation catches that $rankFusion is not the first stage. This test may help
// expose discrepancies across sharding topologies.
assert.commandFailedWithCode(
    runPipeline([{$limit: 10}, {$rankFusion: {input: {pipelines: {nested: [{$sort: {_id: 1}}]}}}}]),
    10170100);