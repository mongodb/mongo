import "jstests/multiVersion/libs/multi_cluster.js";

import {ShardingTest} from "jstests/libs/shardingtest.js";

export function testPerformUpgradeDowngradeSharded({
    setupFn,
    whenFullyDowngraded,
    whenOnlyConfigIsLatestBinary,
    whenSecondariesAndConfigAreLatestBinary,
    whenMongosBinaryIsLastLTS,
    whenBinariesAreLatestAndFCVIsLastLTS,
    whenFullyUpgraded
}) {
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 2},
        mongos: 1,
        config: 1,
        other: {
            mongosOptions: {binVersion: "last-lts"},
            configOptions: {binVersion: "last-lts"},
            rsOptions: {binVersion: "last-lts"}
        },
    });
    st.configRS.awaitReplication();

    setupFn(st.s, st);

    whenFullyDowngraded(st.s);

    const justWaitForStable =
        {upgradeShards: false, upgradeMongos: false, upgradeConfigs: false, waitUntilStable: true};

    // Upgrade the configs.
    st.upgradeCluster('latest', {...justWaitForStable, upgradeConfigs: true});

    whenOnlyConfigIsLatestBinary(st.s);

    // Upgrade the secondary shard.
    st.upgradeCluster('latest', {...justWaitForStable, upgradeOneShard: st.rs1});

    whenSecondariesAndConfigAreLatestBinary(st.s);

    // Upgrade the rest of the cluster.
    st.upgradeCluster('latest', {...justWaitForStable, upgradeShards: true});

    whenMongosBinaryIsLastLTS(st.s);

    // Upgrade mongos.
    st.upgradeCluster('latest', {...justWaitForStable, upgradeMongos: true});

    whenBinariesAreLatestAndFCVIsLastLTS(st.s);

    // Upgrade the FCV.
    assert.commandWorked(st.s.getDB(jsTestName()).adminCommand({
        setFeatureCompatibilityVersion: latestFCV,
        confirm: true
    }));

    whenFullyUpgraded(st.s);

    // Downgrade the FCV without restarting.
    assert.commandWorked(st.s.getDB(jsTestName()).adminCommand({
        setFeatureCompatibilityVersion: lastLTSFCV,
        confirm: true
    }));

    whenBinariesAreLatestAndFCVIsLastLTS(st.s);

    st.stop();
}
