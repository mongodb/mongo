/**
 * Verifies that $rankFusion behaves correctly in FCV upgrade/downgrade scenarios.
 */
import {testPerformUpgradeReplSet} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {testPerformUpgradeSharded} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";
import {
    collName,
    getDB,
    rankFusionPipeline,
    rankFusionPipelineWithScoreDetails,
    setupCollection,
    assertRankFusionAggregateAccepted,
} from "jstests/multiVersion/targetedTestsLastLtsFeatures/query-integration-search/libs/rank_fusion_upgrade_downgrade_utils.js";

const viewName = "rank_fusion_view";

// This is a simple view pipeline that we will attempt to run $rankFusion queries on.
const viewPipeline = [{$match: {_id: {$gt: 0}}}];

// TODO SERVER-108470 Add tests for $rankFusion with multiple input pipelines.

const kUnrecognizedPipelineStageErrorCode = 40324;

function assertRankFusionCompletelyRejected(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // $rankFusion is rejected in a plain aggregation command.
    assert.commandFailedWithCode(db.runCommand({aggregate: collName, pipeline: rankFusionPipeline, cursor: {}}), [
        kUnrecognizedPipelineStageErrorCode,
        ErrorCodes.QueryFeatureNotAllowed,
    ]);

    // $rankFusion with scoreDetails is still rejected.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collName, pipeline: rankFusionPipelineWithScoreDetails, cursor: {}}),
        [kUnrecognizedPipelineStageErrorCode, ErrorCodes.QueryFeatureNotAllowed],
    );

    // View creation is rejected when view pipeline has $rankFusion.
    assert.commandFailedWithCode(db.createView(viewName, collName, rankFusionPipeline), [
        kUnrecognizedPipelineStageErrorCode,
        ErrorCodes.QueryFeatureNotAllowed,
        ErrorCodes.OptionNotSupportedOnView,
    ]);
    assert.commandFailedWithCode(db.createView(viewName, collName, rankFusionPipelineWithScoreDetails), [
        kUnrecognizedPipelineStageErrorCode,
        ErrorCodes.QueryFeatureNotAllowed,
        ErrorCodes.OptionNotSupportedOnView,
    ]);

    // Running $rankFusion against a view is rejected.
    assert.commandWorked(db.createView(viewName, collName, viewPipeline));
    assert.commandFailedWithCode(db.runCommand({aggregate: viewName, pipeline: rankFusionPipeline, cursor: {}}), [
        kUnrecognizedPipelineStageErrorCode,
        ErrorCodes.QueryFeatureNotAllowed,
        ErrorCodes.OptionNotSupportedOnView,
    ]);
}

function assertRankFusionCompletelyAccepted(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    assertRankFusionAggregateAccepted(db, collName);

    // View creation is rejected when view pipeline has $rankFusion.
    assert.commandFailedWithCode(db.createView(viewName, collName, rankFusionPipeline), [
        kUnrecognizedPipelineStageErrorCode,
        ErrorCodes.QueryFeatureNotAllowed,
        ErrorCodes.OptionNotSupportedOnView,
    ]);
    assert.commandFailedWithCode(db.createView(viewName, collName, rankFusionPipelineWithScoreDetails), [
        kUnrecognizedPipelineStageErrorCode,
        ErrorCodes.QueryFeatureNotAllowed,
        ErrorCodes.OptionNotSupportedOnView,
    ]);

    // Running $rankFusion against a view succeeds.
    assert.commandWorked(db.createView(viewName, collName, viewPipeline));
    assert.commandWorked(db.runCommand({aggregate: viewName, pipeline: rankFusionPipeline, cursor: {}}));
    assert.commandWorked(
        db.runCommand({aggregate: viewName, pipeline: rankFusionPipelineWithScoreDetails, cursor: {}}),
    );
}

testPerformUpgradeReplSet({
    setupFn: setupCollection,
    whenFullyDowngraded: assertRankFusionCompletelyRejected,
    whenSecondariesAreLatestBinary: assertRankFusionCompletelyRejected,
    whenBinariesAreLatestAndFCVIsLastLTS: assertRankFusionCompletelyRejected,
    whenFullyUpgraded: assertRankFusionCompletelyAccepted,
});

testPerformUpgradeSharded({
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
