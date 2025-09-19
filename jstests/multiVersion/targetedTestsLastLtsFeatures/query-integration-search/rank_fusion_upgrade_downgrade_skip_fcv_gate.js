/**
 * Verifies that $rankFusion behaves correctly in FCV upgrade/downgrade scenarios with FCV-gating
 * bypass enabled.
 *
 * This test mimics the setup that Atlas will have - i.e., rank fusion feature flags enabled via
 * setParameter on 8.0 and bypassRankFusionFCVGate enabled on upgrade. With this setup we expect
 * that rank fusion features will be available throughout the entire upgrade.
 */
import {
    testPerformUpgradeDowngradeReplSet
} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {
    testPerformUpgradeDowngradeSharded
} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";
import {
    assertRankFusionAggregateAccepted,
    collName,
    getDB,
    setupCollection,
} from
    "jstests/multiVersion/targetedTestsLastLtsFeatures/query-integration-search/libs/rank_fusion_upgrade_downgrade_utils.js";

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
}

/**
 * Verifies that all rank fusion feature flag functionality works as expected.
 */
function assertRankFusionFeaturesAccepted(primaryConn) {
    const db = getDB(primaryConn);

    // Test that $rankFusion with and without scoreDetails is accepted.
    assertRankFusionAggregateAccepted(db, collName);

    assertRefactoredMQLKeepsWorking(primaryConn);
}

function runTest({startingNodeOptions, preBinaryUpgradeFn, postBinaryUpgradeFn}) {
    testPerformUpgradeDowngradeReplSet({
        startingNodeOptions,
        upgradeNodeOptions: {setParameter: {bypassRankFusionFCVGate: true}},
        setupFn: setupCollection,
        whenFullyDowngraded: preBinaryUpgradeFn,
        whenSecondariesAreLatestBinary: preBinaryUpgradeFn,
        whenBinariesAreLatestAndFCVIsLastLTS: postBinaryUpgradeFn,
        whenFullyUpgraded: postBinaryUpgradeFn,
    });

    testPerformUpgradeDowngradeSharded({
        startingNodeOptions,
        upgradeNodeOptions: {setParameter: {bypassRankFusionFCVGate: true}},
        setupFn: setupCollection,
        whenFullyDowngraded: preBinaryUpgradeFn,
        whenOnlyConfigIsLatestBinary: preBinaryUpgradeFn,
        whenSecondariesAndConfigAreLatestBinary: preBinaryUpgradeFn,
        whenMongosBinaryIsLastLTS: preBinaryUpgradeFn,
        whenBinariesAreLatestAndFCVIsLastLTS: postBinaryUpgradeFn,
        whenFullyUpgraded: postBinaryUpgradeFn,
    });
}

// Run the test with and without the feature flags enabled on the previous version. The former gives
// us coverage for how we expect this flag to be used on Atlas. The latter mimics how we expect the
// rollout to happen in Atlas initially (e.g. a non-FCV gated upgrade, where both old and new
// binaries contain the feature and understand new fields, etc).
runTest({
    startingNodeOptions: {},
    preBinaryUpgradeFn: assertRefactoredMQLKeepsWorking,
    postBinaryUpgradeFn: assertRankFusionFeaturesAccepted
});
runTest({
    startingNodeOptions: {
        setParameter: {featureFlagRankFusionBasic: true, featureFlagRankFusionFull: true},
    },
    preBinaryUpgradeFn: assertRankFusionFeaturesAccepted,
    postBinaryUpgradeFn: assertRankFusionFeaturesAccepted
});
