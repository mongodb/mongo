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

const scoreFirstViewDefinition = [
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

const scoreSecondViewDefinition = [
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

assert.commandFailedWithCode(db.createView("scoreView", collName, scoreFirstViewDefinition),
                             ErrorCodes.OptionNotSupportedOnView);

assert.commandFailedWithCode(db.createView("scoreView", collName, scoreSecondViewDefinition),
                             ErrorCodes.OptionNotSupportedOnView);
