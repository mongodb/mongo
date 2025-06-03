/**
 * Test that references to "score" are correctly validated with the $score stage in different
 * relative positions.
 *
 * TODO SERVER-100946 Enable this test to be run in $facets.
 *
 * featureFlagRankFusionBasic is required to enable use of "score".
 * featureFlagSearchHybridScoringFull is required to enable use of $score.
 * @tags: [
 *   featureFlagRankFusionFull,
 *   featureFlagSearchHybridScoringFull,
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
const constantProjectMyScoreStage = {
    $project: {myScore: {$const: 7}}
};
const sortStage = {
    $sort: {_id: 1}
};
const inhibitOptimizationStage = {
    $_internalInhibitOptimization: {}
};

function runPipelineAndCheckExpectedMetaScoreResult(scorePipeline, expectedResultsPipeline) {
    let actualResult = coll.aggregate(scorePipeline).toArray();
    let expectedResult = coll.aggregate(expectedResultsPipeline).toArray();
    assert.eq(actualResult, expectedResult);
}

// Project'ing "score" immediately after $score works.
(function projectScoreAfterScoreStageWorks() {
    runPipelineAndCheckExpectedMetaScoreResult(
        [scoreStage, metaProjectScoreStage, inhibitOptimizationStage, sortStage],
        [constantProjectMyScoreStage, inhibitOptimizationStage, sortStage]);
})();

// Project'ing "score" many stages after $score works.
(function projectScoreManyStagesAfterScoreStageWorks() {
    runPipelineAndCheckExpectedMetaScoreResult(
        [
            scoreStage,
            matchStage,
            skipStage,
            limitStage,
            metaProjectScoreStage,
            inhibitOptimizationStage,
            sortStage
        ],
        [
            matchStage,
            skipStage,
            limitStage,
            constantProjectMyScoreStage,
            inhibitOptimizationStage,
            sortStage
        ]);
})();

// Project'ing "score" works when $score isn't the first stage.
(function projectScoreWhenScoreStageIsNotFirst() {
    runPipelineAndCheckExpectedMetaScoreResult(
        [
            matchStage,
            skipStage,
            scoreStage,
            limitStage,
            metaProjectScoreStage,
            inhibitOptimizationStage,
            sortStage
        ],
        [
            matchStage,
            skipStage,
            limitStage,
            constantProjectMyScoreStage,
            inhibitOptimizationStage,
            sortStage
        ]);
})();

// Project'ing "score" works with multiple stages involved when $score is not a constant.
(function projectScoreWhenScoreStageIsNotConstant() {
    runPipelineAndCheckExpectedMetaScoreResult(
        [
            {$score: {score: "$a", normalizeFunction: "none"}},
            matchStage,
            skipStage,
            limitStage,
            metaProjectScoreStage,
            inhibitOptimizationStage,
            sortStage
        ],
        [
            matchStage,
            skipStage,
            limitStage,
            {$project: {myScore: "$a"}},
            inhibitOptimizationStage,
            sortStage
        ]);
})();

// Project'ing "score" after a $group is rejected since $group drops the per-document metadata.
assert.commandFailedWithCode(db.runCommand({
    aggregate: collName,
    pipeline: [scoreStage, groupStage, metaProjectScoreStage],
    cursor: {}
}),
                             kUnavailableMetadataErrCode);

// Project'ing "score" before the $score stage is rejected.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [matchStage, skipStage, metaProjectScoreStage, limitStage, scoreStage],
    cursor: {}
}),
                             kUnavailableMetadataErrCode);

// Project'ing "score" when there is no score generated is rejected.
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: [metaProjectScoreStage], cursor: {}}),
    kUnavailableMetadataErrCode);
