/**
 * Verifies that $rankFusion behaves correctly in upgrade/downgrade scenarios.
 * 
 * This upgrade/downgrade test is unique because we don't test a binary or FCV upgrade -
 * instead, we restart the cluster while enabling the feature flag with setParameter.
 * This mimics how the feature will be turned on in Atlas.
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    testPerformUpgradeReplSet
} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {
    testPerformUpgradeSharded
} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

const collName = jsTestName();
const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());

const viewName = "rank_fusion_view";
const rankFusionPipeline =
    [{ $rankFusion: { input: { pipelines: { field: [{ $sort: { foo: 1 } }] } } } }];
const rankFusionPipelineWithScoreDetails = [
    { $rankFusion: { input: { pipelines: { field: [{ $sort: { foo: 1 } }] } }, scoreDetails: true } },
    { $project: { scoreDetails: { $meta: "scoreDetails" }, score: { $meta: "score" } } },
];

function setupCollection(primaryConn, shardingTest = null) {
    const coll = assertDropAndRecreateCollection(getDB(primaryConn), collName);

    if (shardingTest) {
        shardingTest.shardColl(coll, {_id: 1});
    }

    assert.commandWorked(
        coll.insertMany([{_id: 0, foo: "xyz"}, {_id: 1, foo: "bar"}, {_id: 2, foo: "mongodb"}]));
    assert.commandWorked(coll.createIndex({ foo: "text" }));
}
// Override to allow running with an unsharded collection in a sharded cluster.
function setupCollectionUnsharded(primaryConn, shardingTest) {
    setupCollection(primaryConn);
}

/**
 * Verifies that existing stages whose behavior depends on the value of the rank fusion feature
 * flags work as expected.
 */
function assertRefactoredMQLKeepsWorking(primaryConn) {
    const db = getDB(primaryConn);

    {
        // Run a $lookup with a $setWindowFields. This covers the case
        // where an upgraded shard requests sort key metadata from a
        // non-upgraded shard.
        const results = db[collName]
            .aggregate([
                {
                    $lookup: {
                        from: collName,
                        pipeline: [{$setWindowFields: {sortBy: {numOccurrences: 1}, output: {rank: {$rank: {}}}}}],
                        as: "out",
                    },
                },
            ])
            .toArray();
        assert.gt(results.length, 0);
        assert.gt(results[0]["out"].length, 0, results);
    }

    {
        // Run a $text query that produces $textScore metadata. This covers
        // the case where shards generate implicit $score metadata before mongos
        // is upgraded.
        const results =
            db[collName]
                .aggregate(
                    [{$match: {$text: {$search: "xyz"}}}, {$sort: {score: {$meta: "textScore"}}}])
                .toArray();
        assert.eq(results, [{_id: 0, foo: "xyz"}]);
    }

    {
        // Run a $setWindowFields with and without optimizations enabled. This covers the case where mongos desugars the
        // pipeline and does not include outputSortKeyMetadata in the $sort, but mongod already has the feature enabled
        // and expects sort key metadata to exist. This specifically causes an issue when the sort is not pushed down.
        const setWindowFields = {$setWindowFields: {partitionBy: "$foo", sortBy: {bar: 1}, output: {rank: {$rank: {}}}}};
        let results = db[collName]
            .aggregate(setWindowFields)
            .toArray();
        assert.gt(results.length, 0);

        results = db[collName]
            .aggregate([
                {$_internalInhibitOptimization: {}},
                setWindowFields
            ])
            .toArray();
        assert.gt(results.length, 0);
    }
}

function assertRankFusionCompletelyRejected(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    assertRefactoredMQLKeepsWorking(primaryConn);

    // $rankFusion is rejected in a plain aggregation command.
    assert.commandFailedWithCode(
        db.runCommand({ aggregate: collName, pipeline: rankFusionPipeline, cursor: {} }),
        [ErrorCodes.QueryFeatureNotAllowed]);

    // $rankFusion with scoreDetails is still rejected.
    assert.commandFailedWithCode(
        db.runCommand(
            { aggregate: collName, pipeline: rankFusionPipelineWithScoreDetails, cursor: {} }),
        [ErrorCodes.QueryFeatureNotAllowed]);

    // View creation is rejected when view pipeline has $rankFusion.
    assert.commandFailedWithCode(db.createView(viewName, collName, rankFusionPipeline), [
        ErrorCodes.QueryFeatureNotAllowed,
        ErrorCodes.OptionNotSupportedOnView
    ]);
    assert.commandFailedWithCode(
        db.createView(viewName, collName, rankFusionPipelineWithScoreDetails), [
        ErrorCodes.QueryFeatureNotAllowed,
        ErrorCodes.OptionNotSupportedOnView
    ]);
}

function assertRankFusionCompletelyAccepted(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    assertRefactoredMQLKeepsWorking(primaryConn);

    // $rankFusion succeeds in an aggregation command.
    assert.commandWorked(
        db.runCommand({ aggregate: collName, pipeline: rankFusionPipeline, cursor: {} }));

    // $rankFusion with scoreDetails succeeds in an aggregation command.
    assert.commandWorked(db.runCommand(
        { aggregate: collName, pipeline: rankFusionPipelineWithScoreDetails, cursor: {} }));

    // $rankFusion cannot be run in a view definition.
    assert.commandFailedWithCode(db.createView(viewName, collName, rankFusionPipeline),
        [ErrorCodes.OptionNotSupportedOnView]);
    assert.commandFailedWithCode(
        db.createView(viewName, collName, rankFusionPipelineWithScoreDetails),
        [ErrorCodes.OptionNotSupportedOnView]);
}

/**
 * Note: we skip the "whenFullyUpgraded" state here because the pre-FCV bump state is equivalent to post bump for our case.
 * The test fixture will test an FCV downgrade without a restart alongside the FCV upgrade, which will not correctly mimic
 * what a downgrade means in this context (i.e. removing the feature flag), so we want to skip that too.
 */

testPerformUpgradeReplSet({
    setupFn: setupCollection,
    startingVersion: {
        binVersion: "latest"
    },
    upgradeNodeOptions: {setParameter: {featureFlagRankFusionBasic: true, featureFlagRankFusionFull: true}},
    whenFullyDowngraded: assertRankFusionCompletelyRejected,
    whenSecondariesAreLatestBinary: assertRankFusionCompletelyRejected,
    whenBinariesAreLatestAndFCVIsLastLTS: assertRankFusionCompletelyAccepted,
});

// Sharded collection in a sharded cluster.
testPerformUpgradeSharded({
    setupFn: setupCollection,
    startingVersion: {
        binVersion: "latest"
    },
    upgradeNodeOptions: {setParameter: {featureFlagRankFusionBasic: true, featureFlagRankFusionFull: true}},
    whenFullyDowngraded: assertRankFusionCompletelyRejected,
    whenOnlyConfigIsLatestBinary: assertRankFusionCompletelyRejected,
    whenSecondariesAndConfigAreLatestBinary: assertRankFusionCompletelyRejected,
    whenMongosBinaryIsLastLTS: assertRankFusionCompletelyRejected,
    whenBinariesAreLatestAndFCVIsLastLTS: assertRankFusionCompletelyAccepted,
});

// Unsharded collection in a sharded cluster.
testPerformUpgradeSharded({
    setupFn: setupCollectionUnsharded,
    startingVersion: {
        binVersion: "latest"
    },
    upgradeNodeOptions: {setParameter: {featureFlagRankFusionBasic: true, featureFlagRankFusionFull: true}},
    whenFullyDowngraded: assertRankFusionCompletelyRejected,
    whenOnlyConfigIsLatestBinary: assertRankFusionCompletelyRejected,
    whenSecondariesAndConfigAreLatestBinary: assertRankFusionCompletelyRejected,
    whenMongosBinaryIsLastLTS: assertRankFusionCompletelyRejected,
    whenBinariesAreLatestAndFCVIsLastLTS: assertRankFusionCompletelyAccepted,
});
