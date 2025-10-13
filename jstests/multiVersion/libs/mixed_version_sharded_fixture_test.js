import "jstests/multiVersion/libs/multi_cluster.js";

import {copyJSON} from "jstests/libs/json_utils.js";

const latestVersion = {
    binVersion: "latest"
};
const lastLTSVersion = {
    binVersion: "last-lts"
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
    setupFn,
    beforeRestart,
    afterConfigHasRestarted,
    afterSecondaryShardHasRestarted,
    afterPrimaryShardHasRestarted,
    afterMongosHasRestarted,
    afterFCVBump = null,
}) {
    // Create a copy of the options each time they're passed since the callees may modify them.
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 2},
        mongos: 1,
        config: 1,
        other: {
            mongosOptions: {...startingVersion, ...copyJSON(startingNodeOptions)},
            configOptions: {...startingVersion, ...copyJSON(startingNodeOptions)},
            rsOptions: {...startingVersion, ...copyJSON(startingNodeOptions)}
        },
    });
    st.configRS.awaitReplication();

    setupFn(st.s, st);

    beforeRestart(st.s);

    const justWaitForStable =
        {upgradeShards: false, upgradeMongos: false, upgradeConfigs: false, waitUntilStable: true};

    // Upgrade the configs.
    st.restartBinariesWithoutUpgradeBackCompat(restartVersion.binVersion,
                      {...justWaitForStable, upgradeConfigs: true},
                      copyJSON(restartNodeOptions));
    afterConfigHasRestarted(st.s);

    // Upgrade the secondary shard.
    st.restartBinariesWithoutUpgradeBackCompat(restartVersion.binVersion,
                      {...justWaitForStable, upgradeOneShard: st.rs1},
                      copyJSON(restartNodeOptions));
    afterSecondaryShardHasRestarted(st.s);

    // Upgrade the rest of the cluster.
    st.restartBinariesWithoutUpgradeBackCompat(restartVersion.binVersion,
                      {...justWaitForStable, upgradeShards: true},
                      copyJSON(restartNodeOptions));
    afterPrimaryShardHasRestarted(st.s);

    // Upgrade mongos.
    st.restartBinariesWithoutUpgradeBackCompat(restartVersion.binVersion,
                      {...justWaitForStable, upgradeMongos: true},
                      copyJSON(restartNodeOptions));
    afterMongosHasRestarted(st.s);

    if (afterFCVBump) {
        // Upgrade the FCV.
        assert.commandWorked(st.s.getDB(jsTestName()).adminCommand({
            setFeatureCompatibilityVersion: latestFCV,
            confirm: true
        }));

        afterFCVBump(st.s);

        // Downgrade FCV without restarting.
        assert.commandWorked(st.s.getDB(jsTestName()).adminCommand({
            setFeatureCompatibilityVersion: lastLTSFCV,
            confirm: true
        }));

        afterMongosHasRestarted(st.s);
    }

    st.stop();
}

export function testPerformUpgradeSharded({
    startingVersion = lastLTSVersion,
    restartVersion = latestVersion,
    startingNodeOptions = {},
    upgradeNodeOptions = {},
    setupFn,
    whenFullyDowngraded,
    whenOnlyConfigIsLatestBinary,
    whenSecondariesAndConfigAreLatestBinary,
    whenMongosBinaryIsLastLTS,
    whenBinariesAreLatestAndFCVIsLastLTS,
    whenFullyUpgraded
}) {
    testPerformShardedClusterRollingRestart({
        startingVersion,
        restartVersion,
        startingNodeOptions,
        restartNodeOptions: upgradeNodeOptions,
        setupFn,
        beforeRestart: whenFullyDowngraded,
        afterConfigHasRestarted: whenOnlyConfigIsLatestBinary,
        afterSecondaryShardHasRestarted: whenSecondariesAndConfigAreLatestBinary,
        afterPrimaryShardHasRestarted: whenMongosBinaryIsLastLTS,
        afterMongosHasRestarted: whenBinariesAreLatestAndFCVIsLastLTS,
        afterFCVBump: whenFullyUpgraded,
    });
}
