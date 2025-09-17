/**
 * This test simulates a scenario when you're upgrading/downgrading an extension between V1 and V2
 * while maintaining the same server version. For the sake of testing, $testFoo in V1 must provide
 * an empty stage definition like {$testFoo: {}} or it will reject the query at parsing; $testFoo in
 * V2 loosens those restrictions. We test this expected behavior for upgrade:
 *     - The nodes are spun up with extension V1 loaded. $testFoo is accepted in queries and views,
 *       but it is always rejected (in queries and views) with a non-empty stage definition.
 *     - In a ReplSet, $testFoo V1 queries must be accepted the whole time. $testFoo V2 queries
 *       are only accepted once all binaries are upgraded (the primary last). Note that we are
 *       missing coverage for ReplSet upgrade/downgrade where we aren't just connected to the
 *       primary the whole time (TODO SERVER-109457).
 *     - In a sharded cluster, again $testFoo V1 queries must be accepted the whole time, and
 *       $testFoo V2 queries are only accepted once all binaries are upgraded (the mongos last).
 *        * However, there is one difference: in the period between when the shards start restarting
 *          until mongos has restarted, the cluster may allow you to create a view with $testFoo V2
 *          even though you cannot run queries on the view until mongos has upgraded. This behavior
 *          is flaky, as it occasionally rejects the view creation to begin with. Note that this
 *          only happens when auth is not enabled (see extension_foo_upgrade_downgrade_auth.js for
 *          testing with more consistent behavior when auth is enabled).
 *     - The downgrade behavior is reverse and more consistent (no flaky view issues).
 *
 * This test is technically not multiversion since we only use latest binaries, but it stays with
 * multiversion tests since we use the upgrade/downgrade library utils.
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
import "jstests/multiVersion/libs/multi_rs.js";
import "jstests/multiVersion/libs/multi_cluster.js";

import {isLinux} from "jstests/libs/os_helpers.js";
import {
    assertFooStageAcceptedV1AndV2,
    assertFooStageAcceptedV1Only,
    assertFooStageAcceptedV1OnlyPlusV2ViewCreation,
    extensionNodeOptions,
    setupCollection,
    generateMultiversionExtensionConfigs,
    deleteMultiversionExtensionConfigs,
} from "jstests/multiVersion/genericBinVersion/extensions_api/libs/extension_foo_upgrade_downgrade_utils.js";
import {testPerformReplSetRollingRestart} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {testPerformShardedClusterRollingRestart} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

if (!isLinux()) {
    jsTest.log.info("Skipping test since extensions are only available on Linux platforms.");
    quit();
}

const extensionPaths = generateMultiversionExtensionConfigs();

try {
    const fooV1Options = extensionNodeOptions(extensionPaths[0]);
    const fooV2Options = extensionNodeOptions(extensionPaths[1]);

    // Test upgrading foo extension in a replica set.
    testPerformReplSetRollingRestart({
        startingNodeOptions: fooV1Options,
        restartNodeOptions: fooV2Options,
        setupFn: setupCollection,
        beforeRestart: assertFooStageAcceptedV1Only,
        afterSecondariesHaveRestarted: assertFooStageAcceptedV1Only,
        afterPrimariesHaveRestarted: assertFooStageAcceptedV1AndV2,
    });

    // Test upgrading foo extension in a sharded cluster.
    testPerformShardedClusterRollingRestart({
        startingNodeOptions: fooV1Options,
        restartNodeOptions: fooV2Options,
        setupFn: setupCollection,
        beforeRestart: assertFooStageAcceptedV1Only,
        afterConfigHasRestarted: assertFooStageAcceptedV1Only,
        afterSecondaryShardHasRestarted: assertFooStageAcceptedV1OnlyPlusV2ViewCreation,
        afterPrimaryShardHasRestarted: assertFooStageAcceptedV1OnlyPlusV2ViewCreation,
        afterMongosHasRestarted: assertFooStageAcceptedV1AndV2,
    });

    // Test downgrading foo extension in a replica set.
    testPerformReplSetRollingRestart({
        startingNodeOptions: fooV2Options,
        restartNodeOptions: fooV1Options,
        setupFn: setupCollection,
        beforeRestart: assertFooStageAcceptedV1AndV2,
        afterSecondariesHaveRestarted: assertFooStageAcceptedV1AndV2,
        afterPrimariesHaveRestarted: assertFooStageAcceptedV1Only,
    });

    // Test downgrading foo extension in a sharded cluster.
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
    deleteMultiversionExtensionConfigs(extensionPaths);
}
