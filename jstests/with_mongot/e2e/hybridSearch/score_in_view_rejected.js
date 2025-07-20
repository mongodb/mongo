/**
 * Tests that $rankFusion in a view definition is always rejected.
 *
 * TODO SERVER-101721 Enable $rankFusion to be run in a view definition.
 *
 * @tags: [featureFlagRankFusionBasic, requires_fcv_81]
 */

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

const scoreFirstPipeline = [
    {
        $score: {
            score: {$add: ["$single", "$double"]},
            normalization: "minMaxScaler",
            weight: 0.5,
            scoreDetails: true,
        },
    },
    {$addFields: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}},
    {$sort: {_id: 1}}
];

const scoreSecondViewPipeline = [
    {$limit: 10},
    {
        $score: {
            score: {$add: ["$single", "$double"]},
            normalization: "minMaxScaler",
            weight: 0.5,
            scoreDetails: true,
        },
    },
    {$addFields: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}},
    {$sort: {_id: 1}}
];

const lookupPipelineWithScoreFirst =
    [{$lookup: {from: collName, as: "matched_docs", pipeline: scoreFirstPipeline}}];
const unionWithPipelineWithScoreFirst =
    [{$unionWith: {coll: collName, pipeline: scoreFirstPipeline}}];

const lookupPipelineWithScoreSecond =
    [{$lookup: {from: collName, as: "matched_docs", pipeline: scoreSecondViewPipeline}}];
const unionWithPipelineWithScoreSecond =
    [{$unionWith: {coll: collName, pipeline: scoreSecondViewPipeline}}];

assert.commandFailedWithCode(db.createView("scoreView", collName, scoreFirstPipeline),
                             ErrorCodes.OptionNotSupportedOnView);

assert.commandFailedWithCode(db.createView("scoreView", collName, scoreSecondViewPipeline),
                             ErrorCodes.OptionNotSupportedOnView);

assert.commandFailedWithCode(db.createView("scoreView", collName, lookupPipelineWithScoreFirst),
                             ErrorCodes.OptionNotSupportedOnView);
assert.commandFailedWithCode(db.createView("scoreView", collName, unionWithPipelineWithScoreFirst),
                             ErrorCodes.OptionNotSupportedOnView);

assert.commandFailedWithCode(db.createView("scoreView", collName, lookupPipelineWithScoreSecond),
                             ErrorCodes.OptionNotSupportedOnView);
assert.commandFailedWithCode(db.createView("scoreView", collName, unionWithPipelineWithScoreSecond),
                             ErrorCodes.OptionNotSupportedOnView);
