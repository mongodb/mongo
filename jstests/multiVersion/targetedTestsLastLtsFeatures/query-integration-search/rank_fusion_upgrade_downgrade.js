/**
 * Verifies that $rankFusion behaves correctly in FCV upgrade/downgrade scenarios.
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
const docs = [{_id: 0, foo: "xyz"}, {_id: 1, foo: "bar"}, {_id: 2, foo: "mongodb"}];

const viewName = "rank_fusion_view";

// This is a simple view pipeline that we will attempt to run $rankFusion queries on.
const viewPipeline = [{$match: {_id: {$gt: 0}}}];

// TODO SERVER-108470 Add tests for $rankFusion with multiple input pipelines.
const rankFusionPipeline = [{$rankFusion: {input: {pipelines: {field: [{$sort: {foo: 1}}]}}}}];
const rankFusionPipelineWithScoreDetails = [
    {$rankFusion: {input: {pipelines: {field: [{$sort: {foo: 1}}]}}, scoreDetails: true}},
    {$project: {scoreDetails: {$meta: "scoreDetails"}, score: {$meta: "score"}}}
];

const kUnrecognizedPipelineStageErrorCode = 40324;
function setupCollection(primaryConn, shardingTest = null) {
    const coll = assertDropAndRecreateCollection(getDB(primaryConn), collName);

    if (shardingTest) {
        shardingTest.shardColl(coll, {_id: 1});
    }

    assert.commandWorked(coll.insertMany(docs));
}

function assertRankFusionCompletelyRejected(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // $rankFusion is rejected in a plain aggregation command.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collName, pipeline: rankFusionPipeline, cursor: {}}),
        [kUnrecognizedPipelineStageErrorCode, ErrorCodes.QueryFeatureNotAllowed]);

    // $rankFusion with scoreDetails is still rejected.
    assert.commandFailedWithCode(
        db.runCommand(
            {aggregate: collName, pipeline: rankFusionPipelineWithScoreDetails, cursor: {}}),
        [kUnrecognizedPipelineStageErrorCode, ErrorCodes.QueryFeatureNotAllowed]);

    // View creation is rejected when view pipeline has $rankFusion.
    assert.commandFailedWithCode(db.createView(viewName, collName, rankFusionPipeline), [
        kUnrecognizedPipelineStageErrorCode,
        ErrorCodes.QueryFeatureNotAllowed,
        ErrorCodes.OptionNotSupportedOnView
    ]);
    assert.commandFailedWithCode(
        db.createView(viewName, collName, rankFusionPipelineWithScoreDetails), [
            kUnrecognizedPipelineStageErrorCode,
            ErrorCodes.QueryFeatureNotAllowed,
            ErrorCodes.OptionNotSupportedOnView
        ]);

    // Running $rankFusion against a view is rejected.
    assert.commandWorked(db.createView(viewName, collName, viewPipeline));
    assert.commandFailedWithCode(
        db.runCommand({aggregate: viewName, pipeline: rankFusionPipeline, cursor: {}}),
        [kUnrecognizedPipelineStageErrorCode, ErrorCodes.OptionNotSupportedOnView]);
}

function assertRankFusionCompletelyAccepted(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // $rankFusion succeeds in an aggregation command.
    assert.commandWorked(
        db.runCommand({aggregate: collName, pipeline: rankFusionPipeline, cursor: {}}));

    // $rankFusion with scoreDetails succeeds in an aggregation command.
    assert.commandWorked(db.runCommand(
        {aggregate: collName, pipeline: rankFusionPipelineWithScoreDetails, cursor: {}}));

    // View creation is rejected when view pipeline has $rankFusion.
    assert.commandFailedWithCode(db.createView(viewName, collName, rankFusionPipeline), [
        kUnrecognizedPipelineStageErrorCode,
        ErrorCodes.QueryFeatureNotAllowed,
        ErrorCodes.OptionNotSupportedOnView
    ]);
    assert.commandFailedWithCode(
        db.createView(viewName, collName, rankFusionPipelineWithScoreDetails), [
            kUnrecognizedPipelineStageErrorCode,
            ErrorCodes.QueryFeatureNotAllowed,
            ErrorCodes.OptionNotSupportedOnView
        ]);

    // Running $rankFusion against a view succeeds.
    assert.commandWorked(db.createView(viewName, collName, viewPipeline));
    assert.commandWorked(
        db.runCommand({aggregate: viewName, pipeline: rankFusionPipeline, cursor: {}}));
    assert.commandWorked(db.runCommand(
        {aggregate: viewName, pipeline: rankFusionPipelineWithScoreDetails, cursor: {}}));
}

testPerformUpgradeDowngradeReplSet({
    setupFn: setupCollection,
    whenFullyDowngraded: assertRankFusionCompletelyRejected,
    whenSecondariesAreLatestBinary: assertRankFusionCompletelyRejected,
    whenBinariesAreLatestAndFCVIsLastLTS: assertRankFusionCompletelyRejected,
    whenFullyUpgraded: assertRankFusionCompletelyAccepted,
});

testPerformUpgradeDowngradeSharded({
    setupFn: setupCollection,
    whenFullyDowngraded: assertRankFusionCompletelyRejected,
    whenOnlyConfigIsLatestBinary: assertRankFusionCompletelyRejected,
    whenSecondariesAndConfigAreLatestBinary: assertRankFusionCompletelyRejected,
    whenMongosBinaryIsLastLTS: assertRankFusionCompletelyRejected,
    // TODO SERVER-108470 This should completely reject $rankFusion, however because mongos is not
    // FCV-aware, it is non-trivial to detect that $rankFusion should be rejected here on the
    // shards.
    whenBinariesAreLatestAndFCVIsLastLTS: assertRankFusionCompletelyAccepted,
    whenFullyUpgraded: assertRankFusionCompletelyAccepted,
});
