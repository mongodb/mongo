/**
 * This is an extension of upgrade_enables_extension_foo.js that only runs in auth scenarios. See
 * the header comment in that file for an explanation about expected behavior.
 *
 * TODO SERVER-109450 Delete this test once auth and non-auth have the same behavior.
 *
 * Extensions are only available on Linux machines.
 * @tags: [
 *   requires_auth,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 * ]
 */
import {isLinux} from "jstests/libs/os_helpers.js";
import {
    setupCollection,
    assertFooViewCreationRejectedAndLegacyVectorSearchUsed,
    assertFooViewCreationAllowedAndExtensionVectorSearchUsed,
    assertFooViewCreationAndVectorSearchBehaviorAfterPrimaryUpgrade,
    assertOnlyRouterHasIFRFlagAndExtensionVectorSearchUsed,
    assertOnlyShardHasIFRFlagAndLegacyVectorSearchUsed,
    assertAllNodesHaveIFRFlagAndExtensionVectorSearchUsed,
    generateUpgradeEnablesExtensionConfigs,
    deleteMultiversionExtensionConfigs,
    wrapOptionsWithStubParserFeatureFlag,
    multipleExtensionNodeOptions,
} from "jstests/multiVersion/genericBinVersion/extensions_api/libs/extension_upgrade_downgrade_utils.js";
import {testPerformUpgradeReplSet} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {testPerformUpgradeSharded} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

if (!isLinux()) {
    jsTest.log.info("Skipping test since extensions are only available on Linux platforms.");
    quit();
}

const extensionNames = generateUpgradeEnablesExtensionConfigs();
const ifrFlags = {
    featureFlagVectorSearchExtension: true,
};

try {
    const fooAndVectorSearchOptions = multipleExtensionNodeOptions([extensionNames[0], extensionNames[1]]);
    const upgradeNodeOptions = wrapOptionsWithStubParserFeatureFlag(fooAndVectorSearchOptions);
    testPerformUpgradeReplSet({
        upgradeNodeOptions,
        ifrFlags,
        setupFn: setupCollection,
        whenFullyDowngraded: assertFooViewCreationRejectedAndLegacyVectorSearchUsed,
        // TODO SERVER-115501 Add validation.
        whenSecondariesAreLatestBinary: () => {},
        whenBinariesAreLatestAndFCVIsLastLTS: assertFooViewCreationAndVectorSearchBehaviorAfterPrimaryUpgrade,
        whenIfrFlagsAreToggled: assertFooViewCreationAllowedAndExtensionVectorSearchUsed,
        whenFullyUpgraded: assertFooViewCreationAllowedAndExtensionVectorSearchUsed,
    });

    testPerformUpgradeSharded({
        upgradeNodeOptions,
        ifrFlags,
        setupFn: setupCollection,
        whenFullyDowngraded: assertFooViewCreationRejectedAndLegacyVectorSearchUsed,
        whenOnlyConfigIsLatestBinary: assertFooViewCreationRejectedAndLegacyVectorSearchUsed,
        whenSecondariesAndConfigAreLatestBinary: assertFooViewCreationRejectedAndLegacyVectorSearchUsed,
        whenMongosBinaryIsLastLTS: assertFooViewCreationRejectedAndLegacyVectorSearchUsed,
        whenBinariesAreLatestAndFCVIsLastLTS: assertFooViewCreationAndVectorSearchBehaviorAfterPrimaryUpgrade,
        whenOnlyRouterHasIFRFlag: assertOnlyRouterHasIFRFlagAndExtensionVectorSearchUsed,
        whenOnlyShardHasIFRFlag: assertOnlyShardHasIFRFlagAndLegacyVectorSearchUsed,
        whenIfrFlagsAreToggled: assertAllNodesHaveIFRFlagAndExtensionVectorSearchUsed,
        whenFullyUpgraded: assertFooViewCreationAllowedAndExtensionVectorSearchUsed,
    });
} finally {
    deleteMultiversionExtensionConfigs(extensionNames);
}
