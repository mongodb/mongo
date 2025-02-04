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

const viewName = "rank_fusion_view";
const rankFusionPipeline = [{$rankFusion: {input: {pipelines: {field: [{$sort: {foo: 1}}]}}}}];

function setupCollection(primaryConn, shardingTest = null) {
    const coll = assertDropAndRecreateCollection(getDB(primaryConn), collName);

    if (shardingTest) {
        shardingTest.shardColl(coll, {_id: 1});
    }

    assert.commandWorked(
        coll.insertMany([{_id: 0, foo: "xyz"}, {_id: 1, foo: "bar"}, {_id: 2, foo: "mongodb"}]));
}

function assertRankFusionCompletelyRejected(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // $rankFusion is rejected in a plain aggregation command.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collName, pipeline: rankFusionPipeline, cursor: {}}),
        [40324, ErrorCodes.QueryFeatureNotAllowed]);

    // View creation is rejected when view pipeline has $rankFusion.
    assert.commandFailedWithCode(db.createView(viewName, collName, rankFusionPipeline),
                                 [40324, ErrorCodes.QueryFeatureNotAllowed]);
}

function assertRankFusionCompletelyAccepted(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // $rankFusion succeeds in an aggregation command.
    assert.commandWorked(
        db.runCommand({aggregate: collName, pipeline: rankFusionPipeline, cursor: {}}));

    // View creation succeeds with $rankFusion in the view pipeline, and queries on that view
    // succeed.
    assert.commandWorked(db.createView(viewName, collName, rankFusionPipeline));
    assert.commandWorked(
        db.runCommand({aggregate: viewName, pipeline: [{$match: {_id: {$gt: 0}}}], cursor: {}}));
}

function assertRankFusionAcceptedButNotInView(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // $rankFusion succeeds in an aggregation command, but view creation is rejection with
    // $rankFusion in the view pipeline.
    assert.commandWorked(
        db.runCommand({aggregate: collName, pipeline: rankFusionPipeline, cursor: {}}));
    assert.commandFailedWithCode(db.createView(viewName, collName, rankFusionPipeline),
                                 [40324, ErrorCodes.QueryFeatureNotAllowed]);
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
    whenBinariesAreLatestAndFCVIsLastLTS: assertRankFusionAcceptedButNotInView,
    whenFullyUpgraded: assertRankFusionCompletelyAccepted,
});
