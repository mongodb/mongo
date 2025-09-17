/**
 * This is an extension of extension_foo_upgrade_downgrade.js that only runs in auth scenarios. See
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
    assertFooStageAcceptedV1AndV2,
    assertFooStageAcceptedV1Only,
    setupCollection,
    extensionNodeOptions,
    generateMultiversionExtensionConfigs,
    deleteMultiversionExtensionConfigs,
} from "jstests/multiVersion/genericBinVersion/extensions_api/libs/extension_foo_upgrade_downgrade_utils.js";
import {testPerformReplSetRollingRestart} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {testPerformShardedClusterRollingRestart} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

if (!isLinux()) {
    jsTest.log.info("Skipping test since extensions are only available on Linux platforms.");
    quit();
}

const extensionNames = generateMultiversionExtensionConfigs();

try {
    const fooV1Options = extensionNodeOptions(extensionNames[0]);
    const fooV2Options = extensionNodeOptions(extensionNames[1]);

    testPerformReplSetRollingRestart({
        startingNodeOptions: fooV1Options,
        restartNodeOptions: fooV2Options,
        setupFn: setupCollection,
        beforeRestart: assertFooStageAcceptedV1Only,
        afterSecondariesHaveRestarted: assertFooStageAcceptedV1Only,
        afterPrimariesHaveRestarted: assertFooStageAcceptedV1AndV2,
    });

    testPerformShardedClusterRollingRestart({
        startingNodeOptions: fooV1Options,
        restartNodeOptions: fooV2Options,
        setupFn: setupCollection,
        beforeRestart: assertFooStageAcceptedV1Only,
        afterConfigHasRestarted: assertFooStageAcceptedV1Only,
        afterSecondaryShardHasRestarted: assertFooStageAcceptedV1Only,
        afterPrimaryShardHasRestarted: assertFooStageAcceptedV1Only,
        afterMongosHasRestarted: assertFooStageAcceptedV1AndV2,
    });

    testPerformReplSetRollingRestart({
        startingNodeOptions: fooV2Options,
        restartNodeOptions: fooV1Options,
        setupFn: setupCollection,
        beforeRestart: assertFooStageAcceptedV1AndV2,
        afterSecondariesHaveRestarted: assertFooStageAcceptedV1AndV2,
        afterPrimariesHaveRestarted: assertFooStageAcceptedV1Only,
    });

    testPerformShardedClusterRollingRestart({
        startingNodeOptions: fooV2Options,
        restartNodeOptions: fooV1Options,
        setupFn: setupCollection,
        beforeRestart: assertFooStageAcceptedV1AndV2,
        afterConfigHasRestarted: assertFooStageAcceptedV1AndV2,
        afterSecondaryShardHasRestarted: assertFooStageAcceptedV1Only,
        afterPrimaryShardHasRestarted: assertFooStageAcceptedV1Only,
        afterMongosHasRestarted: assertFooStageAcceptedV1Only,
    });
} finally {
    deleteMultiversionExtensionConfigs(extensionNames);
}
