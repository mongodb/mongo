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
    assertRankFusionAggregateAccepted,
} from "jstests/multiVersion/genericBinVersion/query-integration-search/libs/rank_fusion_upgrade_downgrade_utils.js";

function assertRankFusionFeaturesAccepted(primaryConn) {
    const db = getDB(primaryConn);

    // Test that $rankFusion with and without scoreDetails is accepted.
    assertRankFusionAggregateAccepted(db, collName);

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
        const results = db[collName]
            .aggregate([{$match: {$text: {$search: "xyz"}}}, {$sort: {score: {$meta: "textScore"}}}])
            .toArray();
        assert.eq(results, [{_id: 0, foo: "xyz"}]);
    }
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
