/**
 * Verifies that $rankFusion behaves correctly in FCV upgrade/downgrade scenarios with FCV-gating bypass enabled.
 *
 * This test mimics the setup that Atlas will have - i.e., rank fusion feature flags enabled via setParameter on
 * 8.0 and bypassRankFusionFCVGate enabled on upgrade. With this setup we expect that rank fusion features will be
 * available throughout the entire upgrade.
 */
import {testPerformUpgradeReplSet} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {testPerformUpgradeSharded} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";
import {
    collName,
    getDB,
    setupCollection,
    setupUnshardedCollection,
    assertRankFusionAggregateAccepted,
    assertRefactoredMQLKeepsWorking,
} from "jstests/multiVersion/genericBinVersion/query-integration-search/libs/rank_fusion_upgrade_downgrade_utils.js";

function assertRankFusionFeaturesAccepted(primaryConn) {
    const db = getDB(primaryConn);

    assertRefactoredMQLKeepsWorking(db);

    // Test that $rankFusion with and without scoreDetails is accepted.
    assertRankFusionAggregateAccepted(db, collName);
}

testPerformUpgradeReplSet({
    startingNodeOptions: {setParameter: {featureFlagRankFusionBasic: true, featureFlagRankFusionFull: true}},
    upgradeNodeOptions: {setParameter: {bypassRankFusionFCVGate: true}},
    setupFn: setupCollection,
    whenFullyDowngraded: assertRankFusionFeaturesAccepted,
    whenSecondariesAreLatestBinary: assertRankFusionFeaturesAccepted,
    whenBinariesAreLatestAndFCVIsLastLTS: assertRankFusionFeaturesAccepted,
    whenFullyUpgraded: assertRankFusionFeaturesAccepted,
});

// Sharded collection in a sharded cluster.
testPerformUpgradeSharded({
    startingNodeOptions: {setParameter: {featureFlagRankFusionBasic: true, featureFlagRankFusionFull: true}},
    upgradeNodeOptions: {setParameter: {bypassRankFusionFCVGate: true}},
    setupFn: setupCollection,
    whenFullyDowngraded: assertRankFusionFeaturesAccepted,
    whenOnlyConfigIsLatestBinary: assertRankFusionFeaturesAccepted,
    whenSecondariesAndConfigAreLatestBinary: assertRankFusionFeaturesAccepted,
    whenMongosBinaryIsLastLTS: assertRankFusionFeaturesAccepted,
    whenBinariesAreLatestAndFCVIsLastLTS: assertRankFusionFeaturesAccepted,
    whenFullyUpgraded: assertRankFusionFeaturesAccepted,
});

// Unsharded collection in a sharded cluster.
testPerformUpgradeSharded({
    startingNodeOptions: {setParameter: {featureFlagRankFusionBasic: true, featureFlagRankFusionFull: true}},
    upgradeNodeOptions: {setParameter: {bypassRankFusionFCVGate: true}},
    setupFn: setupUnshardedCollection,
    whenFullyDowngraded: assertRankFusionFeaturesAccepted,
    whenOnlyConfigIsLatestBinary: assertRankFusionFeaturesAccepted,
    whenSecondariesAndConfigAreLatestBinary: assertRankFusionFeaturesAccepted,
    whenMongosBinaryIsLastLTS: assertRankFusionFeaturesAccepted,
    whenBinariesAreLatestAndFCVIsLastLTS: assertRankFusionFeaturesAccepted,
    whenFullyUpgraded: assertRankFusionFeaturesAccepted,
});
