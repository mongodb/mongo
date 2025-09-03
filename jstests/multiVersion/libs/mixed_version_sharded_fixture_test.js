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
    setupFn,
    beforeRestart,
    afterConfigHasRestarted,
    afterSecondaryShardHasRestarted,
    afterPrimaryShardHasRestarted,
    afterMongosHasRestarted,
    afterFCVBump = null,
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
    setupFn,
    whenFullyDowngraded,
    whenOnlyConfigIsLatestBinary,
    whenSecondariesAndConfigAreLatestBinary,
    whenMongosBinaryIsLastLTS,
    whenBinariesAreLatestAndFCVIsLastLTS,
    whenFullyUpgraded,
}) {
    testPerformShardedClusterRollingRestart({
        startingVersion: lastLTSVersion,
        restartVersion: latestVersion,
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
