/**
 * Tests upgrade/downgrade behavior for the $search and $searchMeta extension stages, including
 * their use inside $unionWith, $lookup, and view definitions.
 *
 * Verifies that the IFR-gated switch between legacy and extension implementations works correctly
 * across rolling upgrades, covering both replica set and sharded cluster topologies.
 *
 * Extensions are only available on Linux machines.
 * @tags: [
 *   auth_incompatible,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 * ]
 */
import {isLinux} from "jstests/libs/os_helpers.js";
import {
    assertAllNodesHaveIFRFlagAndExtensionSearchUsed,
    assertFullExtensionSearchBehavior,
    assertFullLegacySearchBehavior,
    assertFullSearchBehaviorAfterPrimaryUpgrade,
    assertOnlyRouterHasIFRFlagAndExtensionSearchUsed,
    assertOnlyShardHasIFRFlagAndLegacySearchUsed,
    deleteMultiversionExtensionConfigs,
    extensionNodeOptions,
    generateSearchExtensionConfigs,
    setupCollection,
    wrapOptionsWithStubParserFeatureFlag,
    wrapOptionsWithExtensionsInsideHybridSearchDisabled,
    wrapOptionsWithExtensionsInsideHybridSearchEnabled,
} from "jstests/multiVersion/genericBinVersion/extensions_api/libs/extension_upgrade_downgrade_utils.js";
import {testPerformUpgradeReplSet} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {testPerformUpgradeSharded} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

if (!isLinux()) {
    jsTest.log.info("Skipping test since extensions are only available on Linux platforms.");
    quit();
}

const extensionNames = generateSearchExtensionConfigs();
const ifrFlags = {
    featureFlagSearchExtension: true,
};

try {
    for (const extensionUnionWithFeatureFlagValue of [true, false]) {
        for (const isSearchMeta of [false, true]) {
            const stageName = isSearchMeta ? "$searchMeta" : "$search";
            jsTest.log.info(
                "Testing upgrade/downgrade for " +
                    stageName +
                    " with extensionUnionWithFeatureFlag=" +
                    extensionUnionWithFeatureFlagValue,
            );

            let upgradeNodeOptions = wrapOptionsWithStubParserFeatureFlag(
                extensionNodeOptions(extensionNames[0]),
            );
            if (extensionUnionWithFeatureFlagValue) {
                upgradeNodeOptions =
                    wrapOptionsWithExtensionsInsideHybridSearchEnabled(upgradeNodeOptions);
            } else {
                upgradeNodeOptions =
                    wrapOptionsWithExtensionsInsideHybridSearchDisabled(upgradeNodeOptions);
            }

            testPerformUpgradeReplSet({
                upgradeNodeOptions,
                ifrFlags,
                setupFn: setupCollection,
                whenFullyDowngraded: (conn) => assertFullLegacySearchBehavior(conn, isSearchMeta),
                // TODO SERVER-115501 Add validation for mixed-binary replset state.
                whenSecondariesAreLatestBinary: () => {},
                whenBinariesAreLatestAndFCVIsLastLTS: (conn) =>
                    assertFullSearchBehaviorAfterPrimaryUpgrade(conn, isSearchMeta),
                whenIfrFlagsAreToggled: (conn) =>
                    assertFullExtensionSearchBehavior(conn, isSearchMeta),
                whenFullyUpgraded: (conn) => assertFullExtensionSearchBehavior(conn, isSearchMeta),
            });

            testPerformUpgradeSharded({
                upgradeNodeOptions,
                ifrFlags,
                setupFn: setupCollection,
                whenFullyDowngraded: (conn) => assertFullLegacySearchBehavior(conn, isSearchMeta),
                whenOnlyConfigIsLatestBinary: (conn) =>
                    assertFullLegacySearchBehavior(conn, isSearchMeta),
                whenSecondariesAndConfigAreLatestBinary: (conn) =>
                    assertFullLegacySearchBehavior(conn, isSearchMeta),
                whenMongosBinaryIsLastLTS: (conn) =>
                    assertFullLegacySearchBehavior(conn, isSearchMeta),
                whenBinariesAreLatestAndFCVIsLastLTS: (conn) =>
                    assertFullSearchBehaviorAfterPrimaryUpgrade(conn, isSearchMeta),
                whenOnlyRouterHasIFRFlag: (conn, st) =>
                    assertOnlyRouterHasIFRFlagAndExtensionSearchUsed(conn, st, isSearchMeta),
                whenOnlyShardHasIFRFlag: (conn, st) =>
                    assertOnlyShardHasIFRFlagAndLegacySearchUsed(conn, st, isSearchMeta),
                whenIfrFlagsAreToggled: (conn, st) =>
                    assertAllNodesHaveIFRFlagAndExtensionSearchUsed(conn, st, isSearchMeta),
                whenFullyUpgraded: (conn) => assertFullExtensionSearchBehavior(conn, isSearchMeta),
            });
        }
    }
} finally {
    deleteMultiversionExtensionConfigs(extensionNames);
}
