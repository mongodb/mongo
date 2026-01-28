import "jstests/multiVersion/libs/multi_cluster.js";

import {copyJSON} from "jstests/libs/json_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const latestVersion = {
    binVersion: "latest",
};
const lastLTSVersion = {
    binVersion: "last-lts",
};

/**
 * Simulates a rolling restart of a sharded cluster and calls assertion callbacks at each step
 * along the way. The procedure works like this:
 *   1. Spin up a 2-shard sharded cluster with all nodes on startingVersion and startingNodeOptions.
 *   2. Call the setupFn() and beforeRestart() assertions.
 *   3. Restart the config node with restartVersion and restartNodeOptions.
 *   4. Call the afterConfigHasRestarted() assertions.
 *   5. Restart the secondary shard (with restartVersion and restartNodeOptions).
 *   6. Call the afterSecondaryShardHasRestarted() assertions.
 *   7. Restart the primary shard (with restartVersion and restartNodeOptions).
 *   8. Call the afterPrimaryShardHasRestarted() assertions.
 *   7. Restart the mongos (with restartVersion and restartNodeOptions).
 *   8. Call the afterMongosHasRestarted() assertions.
 *   9. [if afterFCVBump is non-null] Upgrade the FCV.
 *   10. [if afterFCVBump is non-null] Call the afterFCVBump() assertions.
 *   11. [if afterFCVBump is non-null] Downgrade the FCV.
 *   12. [if afterFCVBump is non-null] Call the afterMongosHasRestarted() assertions again.
 */
export function testPerformShardedClusterRollingRestart({
    startingVersion = latestVersion,
    restartVersion = latestVersion,
    startingNodeOptions = {},
    restartNodeOptions = {},
    ifrFlags = {},
    setupFn,
    beforeRestart,
    afterConfigHasRestarted,
    afterSecondaryShardHasRestarted,
    afterPrimaryShardHasRestarted,
    afterMongosHasRestarted,
    afterIfrFlagToggle = null,
    afterFCVBump = null,
    whenOnlyRouterHasIFRFlag = null,
    whenOnlyShardHasIFRFlag = null,
}) {
    jsTest.log.info(
        "Starting a 2-shard cluster with all nodes on version " +
            tojsononeline(startingVersion) +
            " with options " +
            tojsononeline(startingNodeOptions),
    );
    // Create a copy of the options each time they're passed since the callees may modify them.
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 2},
        mongos: 1,
        config: 1,
        other: {
            mongosOptions: {...startingVersion, ...copyJSON(startingNodeOptions)},
            configOptions: {...startingVersion, ...copyJSON(startingNodeOptions)},
            rsOptions: {...startingVersion, ...copyJSON(startingNodeOptions)},
        },
    });
    st.configRS.awaitReplication();

    jsTest.log.info("Calling the setup function");
    setupFn(st.s, st);

    jsTest.log.info("Calling the beforeRestart function");
    beforeRestart(st.s);

    const justWaitForStable = {
        upgradeShards: false,
        upgradeMongos: false,
        upgradeConfigs: false,
        waitUntilStable: true,
    };

    // Upgrade the config server.
    jsTest.log.info(
        "Restarting the config server with version " +
            tojsononeline(restartVersion) +
            " and options " +
            tojsononeline(restartNodeOptions),
    );
    st.upgradeCluster(
        restartVersion.binVersion,
        {...justWaitForStable, upgradeConfigs: true},
        copyJSON(restartNodeOptions),
    );

    jsTest.log.info("Calling the afterConfigHasRestarted function");
    afterConfigHasRestarted(st.s);

    // Upgrade the secondary shard.
    jsTest.log.info(
        "Restarting the secondary shard with version " +
            tojsononeline(restartVersion) +
            " and options " +
            tojsononeline(restartNodeOptions),
    );
    st.upgradeCluster(
        restartVersion.binVersion,
        {...justWaitForStable, upgradeOneShard: st.rs1},
        copyJSON(restartNodeOptions),
    );

    jsTest.log.info("Calling the afterSecondaryShardHasRestarted function");
    afterSecondaryShardHasRestarted(st.s);

    // Upgrade the rest of the cluster.
    jsTest.log.info(
        "Restarting the primary shard with version " +
            tojsononeline(restartVersion) +
            " and options " +
            tojsononeline(restartNodeOptions),
    );
    st.upgradeCluster(
        restartVersion.binVersion,
        {...justWaitForStable, upgradeShards: true},
        copyJSON(restartNodeOptions),
    );

    jsTest.log.info("Calling the afterPrimaryShardHasRestarted function");
    afterPrimaryShardHasRestarted(st.s);

    // Upgrade mongos.
    jsTest.log.info(
        "Restarting the mongos with version " +
            tojsononeline(restartVersion) +
            " and options " +
            tojsononeline(restartNodeOptions),
    );
    st.upgradeCluster(
        restartVersion.binVersion,
        {...justWaitForStable, upgradeMongos: true},
        copyJSON(restartNodeOptions),
    );

    jsTest.log.info("Calling the afterMongosHasRestarted function");
    afterMongosHasRestarted(st.s);

    // We test scenarios in which the router is toggled first and in which the shards are toggled first.
    // This is because the order of nodes on which IFR flags are toggled is not defined - so we
    // want to test all possible scenarios that we could potentially end up in.
    if (whenOnlyRouterHasIFRFlag) {
        jsTest.log.info("Testing when only router has IFR flag enabled");
        const adminDb = st.s.getDB("admin");
        const shard0Admin = st.rs0.getPrimary().getDB("admin");
        const shard1Admin = st.rs1.getPrimary().getDB("admin");

        // Enable flag on router only.
        for (const [flagName, flagValue] of Object.entries(ifrFlags)) {
            assert.commandWorked(adminDb.runCommand({setParameter: 1, [flagName]: flagValue}));
            // Disable flag on shards.
            assert.commandWorked(shard0Admin.runCommand({setParameter: 1, [flagName]: false}));
            assert.commandWorked(shard1Admin.runCommand({setParameter: 1, [flagName]: false}));
        }

        jsTest.log.info("Calling the whenOnlyRouterHasIFRFlag function");
        whenOnlyRouterHasIFRFlag(st.s, st);
    }

    if (whenOnlyShardHasIFRFlag) {
        jsTest.log.info("Testing when only shard has IFR flag enabled");
        const adminDb = st.s.getDB("admin");
        const shard0Admin = st.rs0.getPrimary().getDB("admin");
        const shard1Admin = st.rs1.getPrimary().getDB("admin");

        // Disable flag on router.
        for (const [flagName, flagValue] of Object.entries(ifrFlags)) {
            assert.commandWorked(adminDb.runCommand({setParameter: 1, [flagName]: false}));
            // Enable flag on the shards.
            assert.commandWorked(shard0Admin.runCommand({setParameter: 1, [flagName]: flagValue}));
            assert.commandWorked(shard1Admin.runCommand({setParameter: 1, [flagName]: flagValue}));
        }

        jsTest.log.info("Calling the whenOnlyShardHasIFRFlag function");
        whenOnlyShardHasIFRFlag(st.s, st);
    }

    if (afterIfrFlagToggle) {
        // Enable IFR flags on all nodes (router and shards).
        jsTest.log.info("Enabling IFR flags on all nodes");
        const adminDb = st.s.getDB("admin");
        const shard0Admin = st.rs0.getPrimary().getDB("admin");
        const shard1Admin = st.rs1.getPrimary().getDB("admin");

        for (const [flagName, flagValue] of Object.entries(ifrFlags)) {
            assert.commandWorked(adminDb.runCommand({setParameter: 1, [flagName]: flagValue}));
            assert.commandWorked(shard0Admin.runCommand({setParameter: 1, [flagName]: flagValue}));
            assert.commandWorked(shard1Admin.runCommand({setParameter: 1, [flagName]: flagValue}));
        }

        jsTest.log.info("Calling the afterIfrFlagToggle function");
        afterIfrFlagToggle(st.s, st);
    }

    if (afterFCVBump) {
        // Upgrade the FCV.
        jsTest.log.info("Upgrading the FCV to " + tojsononeline(latestFCV));
        assert.commandWorked(
            st.s.getDB(jsTestName()).adminCommand({
                setFeatureCompatibilityVersion: latestFCV,
                confirm: true,
            }),
        );

        jsTest.log.info("Calling the afterFCVBump function");
        afterFCVBump(st.s);

        // Downgrade FCV without restarting.
        jsTest.log.info("Downgrading the FCV to " + tojsononeline(lastLTSFCV));
        assert.commandWorked(
            st.s.getDB(jsTestName()).adminCommand({
                setFeatureCompatibilityVersion: lastLTSFCV,
                confirm: true,
            }),
        );

        jsTest.log.info("Calling the afterMongosHasRestarted function");
        afterMongosHasRestarted(st.s);
    }

    st.stop();
}

export function testPerformUpgradeSharded({
    startingNodeOptions = {},
    upgradeNodeOptions = {},
    ifrFlags = {},
    setupFn,
    whenFullyDowngraded,
    whenOnlyConfigIsLatestBinary,
    whenSecondariesAndConfigAreLatestBinary,
    whenMongosBinaryIsLastLTS,
    whenBinariesAreLatestAndFCVIsLastLTS,
    whenIfrFlagsAreToggled,
    whenFullyUpgraded,
    whenOnlyRouterHasIFRFlag = null,
    whenOnlyShardHasIFRFlag = null,
}) {
    testPerformShardedClusterRollingRestart({
        startingVersion: lastLTSVersion,
        restartVersion: latestVersion,
        startingNodeOptions,
        restartNodeOptions: upgradeNodeOptions,
        ifrFlags: ifrFlags,
        setupFn,
        beforeRestart: whenFullyDowngraded,
        afterConfigHasRestarted: whenOnlyConfigIsLatestBinary,
        afterSecondaryShardHasRestarted: whenSecondariesAndConfigAreLatestBinary,
        afterPrimaryShardHasRestarted: whenMongosBinaryIsLastLTS,
        afterMongosHasRestarted: whenBinariesAreLatestAndFCVIsLastLTS,
        afterIfrFlagToggle: whenIfrFlagsAreToggled,
        afterFCVBump: whenFullyUpgraded,
        whenOnlyRouterHasIFRFlag: whenOnlyRouterHasIFRFlag,
        whenOnlyShardHasIFRFlag: whenOnlyShardHasIFRFlag,
    });
}

/**
 * Simulates a granular config server upgrade using individual node upgrades and calls assertion
 * callbacks at each step. This enables testing mixed-version config server scenarios.
 * The procedure works like this:
 *   1. Spin up a sharded cluster with 3 config servers on startingVersion.
 *   2. Call setupFn() and beforeRestart() assertions.
 *   3. Upgrade config server at index 0 with restartVersion.
 *   4. Call afterFirstConfigUpgraded() assertions.
 *   5. Upgrade config server at index 1 with restartVersion.
 *   6. Call afterSecondConfigUpgraded() assertions.
 *   7. Upgrade config server at index 2 with restartVersion.
 *   8. Call afterAllConfigsUpgraded() assertions.
 */
export function testPerformIndividualConfigServerUpgrade({
    startingVersion = latestVersion,
    restartVersion = latestVersion,
    startingNodeOptions = {},
    restartNodeOptions = {},
    setupFn,
    beforeRestart,
    afterFirstConfigUpgraded,
    afterSecondConfigUpgraded,
    afterAllConfigsUpgraded,
}) {
    jsTest.log.info(
        "Starting a sharded cluster with 3 config servers on version " +
            tojsononeline(startingVersion) +
            " with options " +
            tojsononeline(startingNodeOptions),
    );

    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 3,
        other: {
            mongosOptions: {...startingVersion, ...copyJSON(startingNodeOptions)},
            configOptions: {...startingVersion, ...copyJSON(startingNodeOptions)},
            rsOptions: {...startingVersion, ...copyJSON(startingNodeOptions)},
        },
    });
    st.configRS.awaitReplication();

    jsTest.log.info("Calling the setup function");
    setupFn(st.s, st);

    jsTest.log.info("Calling the beforeRestart function");
    beforeRestart(st.s);

    const justWaitForStable = {
        upgradeShards: false,
        upgradeMongos: false,
        upgradeConfigs: false,
        waitUntilStable: true,
    };

    // Upgrade first config server
    jsTest.log.info(
        "Upgrading config server at index 0 with version " +
            tojsononeline(restartVersion) +
            " and options " +
            tojsononeline(restartNodeOptions),
    );
    st.upgradeCluster(
        restartVersion.binVersion,
        {...justWaitForStable, upgradeOneConfigNode: 0},
        copyJSON(restartNodeOptions),
    );

    jsTest.log.info("Calling the afterFirstConfigUpgraded function");
    afterFirstConfigUpgraded(st.s);

    // Upgrade second config server
    jsTest.log.info(
        "Upgrading config server at index 1 with version " +
            tojsononeline(restartVersion) +
            " and options " +
            tojsononeline(restartNodeOptions),
    );
    st.upgradeCluster(
        restartVersion.binVersion,
        {...justWaitForStable, upgradeOneConfigNode: 1},
        copyJSON(restartNodeOptions),
    );

    jsTest.log.info("Calling the afterSecondConfigUpgraded function");
    afterSecondConfigUpgraded(st.s);

    // Upgrade third config server
    jsTest.log.info(
        "Upgrading config server at index 2 with version " +
            tojsononeline(restartVersion) +
            " and options " +
            tojsononeline(restartNodeOptions),
    );
    st.upgradeCluster(
        restartVersion.binVersion,
        {...justWaitForStable, upgradeOneConfigNode: 2},
        copyJSON(restartNodeOptions),
    );

    jsTest.log.info("Calling the afterAllConfigsUpgraded function");
    afterAllConfigsUpgraded(st.s);

    // Ensure config replica set is stable before stopping
    jsTest.log.info("Ensuring config replica set is stable before cleanup...");
    st.configRS.awaitReplication();
    st.configRS.awaitSecondaryNodes();
    let configPrimary = st.configRS.getPrimary();
    assert.contains(configPrimary, st.configRS.nodes, "Config primary not found in configRS.nodes");

    st.stop();
}

export function testPerformIndividualConfigUpgrade({
    startingNodeOptions = {},
    restartNodeOptions = {},
    setupFn,
    beforeRestart,
    afterFirstConfigUpgraded,
    afterSecondConfigUpgraded,
    afterAllConfigsUpgraded,
}) {
    testPerformIndividualConfigServerUpgrade({
        startingVersion: lastLTSVersion,
        restartVersion: latestVersion,
        startingNodeOptions,
        restartNodeOptions,
        setupFn,
        beforeRestart,
        afterFirstConfigUpgraded,
        afterSecondConfigUpgraded,
        afterAllConfigsUpgraded,
    });
}
