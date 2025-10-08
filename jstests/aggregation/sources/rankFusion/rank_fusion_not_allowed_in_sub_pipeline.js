/**
 * Tests that $rankFusion is not allowed in a sub-pipeline.
 * @tags: [ featureFlagRankFusionBasic, featureFlagRankFusionFull ]
 */
const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

function assertRankFusionNotAllowedInSubPipeline(pipeline) {
    assert.commandFailedWithCode(db.runCommand({ aggregate: collName, pipeline, cursor: {} }), 11178500);
}

const rankFusionStage = {
    $rankFusion: {
        input: {
            pipelines: {
                x: [
                    { $sort: { x: 1 } },
                ],
                y: [
                    { $sort: { y: 1 } },
                ],
            },
        },
    },
};

// Top-level $lookup.
assertRankFusionNotAllowedInSubPipeline([{$lookup: {from: collName, as: "docs", pipeline: [rankFusionStage]}}]);
// Nested $lookup.
assertRankFusionNotAllowedInSubPipeline([{$lookup: {from: collName, as: "docs1", pipeline: [{$lookup: {from: collName, as: "docs2", pipeline: [rankFusionStage]}}]}}]);
// $unionWith nested in $lookup.
assertRankFusionNotAllowedInSubPipeline([{$lookup: {from: collName, as: "docs1", pipeline: [{$unionWith: {coll: collName, pipeline: [rankFusionStage]}}]}}]);
// Top-level $unionWith.
assertRankFusionNotAllowedInSubPipeline([{$unionWith: {coll: collName, pipeline: [rankFusionStage]}}]);
// Nested $unionWith.
assertRankFusionNotAllowedInSubPipeline([{$unionWith: {coll: collName, pipeline: [{$unionWith: {coll: collName, pipeline: [rankFusionStage]}}]}}]);
// $lookup nested in $unionWith.
assertRankFusionNotAllowedInSubPipeline([{$unionWith: {coll: collName, pipeline: [{$lookup: {from: collName, as: "docs", pipeline: [rankFusionStage]}}]}}]);
