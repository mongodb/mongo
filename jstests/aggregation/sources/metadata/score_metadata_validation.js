/**
 * Test that if the references to "score" are correctly validated with the $score stage in different
 * relative positions.
 *
 * TODO SERVER-97341: This test only checks if the query succeeds or not. We should also test (here
 * or in another file) that the output is correct.
 *
 * TODO SERVER-100946 Enable this test to be run in $facets.
 *
 * featureFlagRankFusionBasic is required to enable use of "score".
 * featureFlagHybridScoringFull is required to enable use of $score.
 * @tags: [
 *   featureFlagRankFusionFull,
 *   featureFlagHybridScoringFull,
 *   do_not_wrap_aggregations_in_facets
 *  ]
 */

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(
    coll.insertMany([{_id: 0, a: 3}, {_id: 1, a: 4}, {_id: 2, a: 5}, {_id: 3, a: -1}]));

const kUnavailableMetadataErrCode = 40218;

const scoreStage = {
    $score: {score: 7, normalizeFunction: "none"}
};
const metaProjectScoreStage = {
    $project: {myScore: {$meta: "score"}}
};
const matchStage = {
    $match: {a: {$gt: 0}}
};
const skipStage = {
    $skip: 1
};
const limitStage = {
    $limit: 2
};
const groupStage = {
    $group: {_id: null, myField: {$max: "$a"}}
};

// Project'ing "score" immediately after $score works.
assert.commandWorked(db.runCommand(
    {aggregate: collName, pipeline: [scoreStage, metaProjectScoreStage], cursor: {}}));

// Project'ing "score" many stages after $score works.
assert.commandWorked(db.runCommand({
    aggregate: collName,
    pipeline: [scoreStage, matchStage, skipStage, limitStage, metaProjectScoreStage],
    cursor: {}
}));

// Project'ing "score" works when $score isn't the first stage.
assert.commandWorked(db.runCommand({
    aggregate: collName,
    pipeline: [matchStage, skipStage, scoreStage, limitStage, metaProjectScoreStage],
    cursor: {}
}));

// TODO SERVER-40900 / SERVER-100443: Project'ing "score" after a $group should raise an error since
// $group drops the per-document metadata, but it curently passes.
assert.commandWorked(db.runCommand(
    {aggregate: collName, pipeline: [scoreStage, groupStage, metaProjectScoreStage], cursor: {}}));

// Project'ing "score" before the $score stage is rejected.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [matchStage, skipStage, metaProjectScoreStage, limitStage, scoreStage],
    cursor: {}
}),
                             kUnavailableMetadataErrCode);

// Project'ing "score" when there is no score generated is rejected.
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: coll.getName(), pipeline: [{$project: {score: {$meta: "score"}}}], cursor: {}}),
    kUnavailableMetadataErrCode);
