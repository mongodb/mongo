/**
 * This test simulates a scenario where you're upgrading from a MongoDB version with no extensions
 * to a loaded to a higher MongoDB version with an extension loaded. We test this expected behavior:
 *    - The nodes are spun up at a lower version with no extensions loaded. All $testFoo queries
 *      should be rejected, both in an agg command and in a createView pipeline.
 *    - In a ReplSet, $testFoo queries should be accepted once all binaries are upgraded (the
 *      primary last). Nothing is dependent on FCV. Note that we are missing coverage for ReplSet
 *      upgrade/downgrade where we aren't just connected to the primary the whole time
 *      (TODO SERVER-109457).
 *    - In a sharded cluster, $testFoo queries should be accepted once all binaries are upgraded
 *      (the mongos last), and again nothing is dependent on FCV.
 *       * However, there is one difference: in the period between when the shards start restarting
 *         until mongos has restarted, the cluster may allow you to create a view with $testFoo
 *         even though you cannot run queries on the view until mongos has upgraded. This behavior
 *         is flaky, as it occasionally rejets the view creation to begin with. Note that this only
 *         happens when auth is not enabed (see upgrade_enables_extension_foo_auth.js for testing
 *         with more consistent behavior when auth is enabled).
 *
 * TODO SERVER-109457 Investigate more ReplSet scenarios.
 *
 * TODO SERVER-109450 Auth-off cluster should have same behavior as auth-on cluster.
 * Extensions are only available on Linux machines.
 * @tags: [
 *   auth_incompatible,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 * ]
 */
import {isLinux} from "jstests/libs/os_helpers.js";
import {
    assertFooStageAccepted,
    assertFooStageRejected,
    assertFooViewCreationAllowedButQueriesRejected,
    setupCollection,
} from "jstests/multiVersion/genericBinVersion/extensions_api/libs/upgrade_enables_extension_foo_utils.js";
import {
    extensionNodeOptions,
    generateMultiversionExtensionConfigs,
    deleteMultiversionExtensionConfigs,
} from "jstests/multiVersion/genericBinVersion/extensions_api/libs/extension_foo_upgrade_downgrade_utils.js";
import {testPerformUpgradeReplSet} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {testPerformUpgradeSharded} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

if (!isLinux()) {
    jsTest.log.info("Skipping test since extensions are only available on Linux platforms.");
    quit();
}

const extensionNames = generateMultiversionExtensionConfigs();

try {
    const fooOptions = extensionNodeOptions(extensionNames[0]);

    testPerformUpgradeReplSet({
        upgradeNodeOptions: fooOptions,
        setupFn: setupCollection,
        whenFullyDowngraded: assertFooStageRejected,
        whenSecondariesAreLatestBinary: assertFooStageRejected,
        whenBinariesAreLatestAndFCVIsLastLTS: assertFooStageAccepted,
        whenFullyUpgraded: assertFooStageAccepted,
    });

    testPerformUpgradeSharded({
        upgradeNodeOptions: fooOptions,
        setupFn: setupCollection,
        whenFullyDowngraded: assertFooStageRejected,
        whenOnlyConfigIsLatestBinary: assertFooStageRejected,
        whenSecondariesAndConfigAreLatestBinary: assertFooViewCreationAllowedButQueriesRejected,
        whenMongosBinaryIsLastLTS: assertFooViewCreationAllowedButQueriesRejected,
        whenBinariesAreLatestAndFCVIsLastLTS: assertFooStageAccepted,
        whenFullyUpgraded: assertFooStageAccepted,
    });
} finally {
    deleteMultiversionExtensionConfigs(extensionNames);
}
