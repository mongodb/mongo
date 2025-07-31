/**
 * Verifies that $scoreFusion behaves correctly in FCV upgrade/downgrade scenarios.
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    testPerformUpgradeDowngradeReplSet
} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {
    testPerformUpgradeDowngradeSharded
} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

const collName = jsTestName();
const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());
const viewName = collName + "_view";

const scoreFusionPipeline = [{
    $scoreFusion: {
        input: {
            pipelines:
                {otherField: [{$score: {score: "$bar"}}], field: [{$score: {score: "$foo"}}]},
            normalization: "minMaxScaler"
        }
    }
}];
const scoreFusionPipelineWithScoreDetails = [{
    $scoreFusion: {
        input: {
            pipelines:
                {otherField: [{$score: {score: "$bar"}}], field: [{$score: {score: "$foo"}}]},
            normalization: "none",
        },
        scoreDetails: true
    }
}];

// This is a simple view pipeline that we will attempt to run $scoreFusion queries on.
const viewPipeline = [{$match: {_id: {$gt: 0}}}];

const scorePipeline = [{$score: {score: "$foo"}}];
const scorePipelineWithScoreDetails = [{$score: {score: "$foo", scoreDetails: true}}];

const projectStage = {
    $project: {scoreDetails: {$meta: "scoreDetails"}, score: {$meta: "score"}}
};
const scoreFusionPipelineWithProject = [...scoreFusionPipelineWithScoreDetails, projectStage];
const scorePipelineWithProject = [...scorePipelineWithScoreDetails, projectStage];

const kUnrecognizedPipelineStageErrorCode = 40324;

function setupCollection(primaryConn, shardingTest = null) {
    const coll = assertDropAndRecreateCollection(getDB(primaryConn), collName);

    if (shardingTest) {
        shardingTest.shardColl(coll, {_id: 1});
    }

    assert.commandWorked(coll.insertMany(
        [{_id: 0, foo: 5, bar: 10}, {_id: 1, foo: 6, bar: 20}, {_id: 2, foo: 7, bar: 30}]));
}

function assertScoreFusionCompletelyRejected(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // Creating a view with $scoreFusion/$score is rejected.
    assert.commandFailedWithCode(
        db.createView("bad_score_fusion_view", collName, scoreFusionPipeline),
        [kUnrecognizedPipelineStageErrorCode, ErrorCodes.OptionNotSupportedOnView]);
    assert.commandFailedWithCode(
        db.createView("bad_score_view", collName, scorePipeline),
        [kUnrecognizedPipelineStageErrorCode, ErrorCodes.OptionNotSupportedOnView]);

    // $scoreFusion/$score on a view is rejected.
    assert.commandWorked(db.createView(viewName, collName, viewPipeline));
    assert.commandFailedWithCode(
        db.runCommand({aggregate: viewName, pipeline: scoreFusionPipeline, cursor: {}}), [
            kUnrecognizedPipelineStageErrorCode,
            ErrorCodes.QueryFeatureNotAllowed,
            ErrorCodes.OptionNotSupportedOnView,
            ErrorCodes.FailedToParse
        ]);
    assert.commandFailedWithCode(
        db.runCommand({aggregate: viewName, pipeline: scorePipeline, cursor: {}}), [
            kUnrecognizedPipelineStageErrorCode,
            ErrorCodes.QueryFeatureNotAllowed,
            ErrorCodes.OptionNotSupportedOnView,
            ErrorCodes.FailedToParse
        ]);

    // $projects referencing score and scoreDetails metadata are rejected in aggregation
    // commands.
    assert.commandFailedWithCode(db.runCommand({
        aggregate: collName,
        pipeline: [...scorePipelineWithScoreDetails, projectStage],
        cursor: {}
    }),
                                 [
                                     ErrorCodes.FailedToParse,
                                     ErrorCodes.QueryFeatureNotAllowed,
                                     kUnrecognizedPipelineStageErrorCode,
                                     17308
                                 ]);
    assert.commandFailedWithCode(db.runCommand({
        aggregate: collName,
        pipeline: [...scorePipelineWithScoreDetails, projectStage],
        cursor: {}
    }),
                                 [
                                     ErrorCodes.FailedToParse,
                                     ErrorCodes.QueryFeatureNotAllowed,
                                     kUnrecognizedPipelineStageErrorCode,
                                     17308
                                 ]);

    // $scoreFusion is rejected in a plain aggregation command.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collName, pipeline: scoreFusionPipeline, cursor: {}}), [
            kUnrecognizedPipelineStageErrorCode,
            ErrorCodes.FailedToParse,
            ErrorCodes.QueryFeatureNotAllowed,
        ]);

    // $scoreFusion with scoreDetails is still rejected.
    assert.commandFailedWithCode(
        db.runCommand(
            {aggregate: collName, pipeline: scoreFusionPipelineWithScoreDetails, cursor: {}}),
        [
            kUnrecognizedPipelineStageErrorCode,
            ErrorCodes.FailedToParse,
            ErrorCodes.QueryFeatureNotAllowed,
        ]);

    // $score is rejected in a plain aggregation command.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collName, pipeline: scorePipeline, cursor: {}}), [
            kUnrecognizedPipelineStageErrorCode,
            ErrorCodes.FailedToParse,
            ErrorCodes.QueryFeatureNotAllowed,
        ]);

    // $score with scoreDetails is still rejected.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collName, pipeline: scorePipelineWithScoreDetails, cursor: {}}), [
            kUnrecognizedPipelineStageErrorCode,
            ErrorCodes.FailedToParse,
            ErrorCodes.QueryFeatureNotAllowed,
        ]);
}

function assertScoreFusionCompletelyAccepted(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // Creating a view with $scoreFusion/$score is rejected.
    assert.commandFailedWithCode(
        db.createView("bad_score_fusion_view", collName, scoreFusionPipeline),
        [ErrorCodes.OptionNotSupportedOnView]);
    assert.commandFailedWithCode(db.createView("bad_score_view", collName, scorePipeline),
                                 [ErrorCodes.OptionNotSupportedOnView]);

    // $scoreFusion/$score on a view works.
    assert.commandWorked(db.createView(viewName, collName, viewPipeline));
    assert.commandWorked(
        db.runCommand({aggregate: viewName, pipeline: scoreFusionPipeline, cursor: {}}));
    assert.commandWorked(db.runCommand({aggregate: viewName, pipeline: scorePipeline, cursor: {}}));

    // $projects referencing score and scoreDetails metadata succeed in aggregation commands.
    assert.commandWorked(
        db.runCommand({aggregate: collName, pipeline: scoreFusionPipelineWithProject, cursor: {}}));
    assert.commandWorked(
        db.runCommand({aggregate: collName, pipeline: scorePipelineWithProject, cursor: {}}));

    // $scoreFusion succeeds in an aggregation command.
    assert.commandWorked(
        db.runCommand({aggregate: collName, pipeline: scoreFusionPipeline, cursor: {}}));

    // $scoreFusion with scoreDetails succeeds in an aggregation command.
    assert.commandWorked(db.runCommand(
        {aggregate: collName, pipeline: scoreFusionPipelineWithScoreDetails, cursor: {}}));

    // $score succeeds in an aggregation command.
    assert.commandWorked(db.runCommand({aggregate: collName, pipeline: scorePipeline, cursor: {}}));

    // $score with scoreDetails succeeds in an aggregation command.
    assert.commandWorked(
        db.runCommand({aggregate: collName, pipeline: scorePipelineWithScoreDetails, cursor: {}}));
}

testPerformUpgradeDowngradeReplSet({
    setupFn: setupCollection,
    whenFullyDowngraded: assertScoreFusionCompletelyRejected,
    whenSecondariesAreLatestBinary: assertScoreFusionCompletelyRejected,
    whenBinariesAreLatestAndFCVIsLastLTS: assertScoreFusionCompletelyRejected,
    whenFullyUpgraded: assertScoreFusionCompletelyAccepted,
});

testPerformUpgradeDowngradeSharded({
    setupFn: setupCollection,
    whenFullyDowngraded: assertScoreFusionCompletelyRejected,
    whenOnlyConfigIsLatestBinary: assertScoreFusionCompletelyRejected,
    whenSecondariesAndConfigAreLatestBinary: assertScoreFusionCompletelyRejected,
    whenMongosBinaryIsLastLTS: assertScoreFusionCompletelyRejected,
    whenBinariesAreLatestAndFCVIsLastLTS: assertScoreFusionCompletelyRejected,
    whenFullyUpgraded: assertScoreFusionCompletelyAccepted,
});
