/**
 * Verify that some metadata is properly changed after the upgrade and downgrade of a sharded
 * cluster. More specifically:
 *
 *   1. Create a sharded cluster in replica set running an old binary version
 *   2. Setup some data on cluster
 *   3. Upgrade binaries and FCV of the cluster to the latest version
 *   4. Verify the data consistency after the upgrade procedure
 *   5. Downgrade binaries and FCV of the cluster to an old version
 *   6. Verify the data consistency after the downgrade procedure
 */
import "jstests/multiVersion/libs/multi_cluster.js";

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();

function setupClusterAndDatabase(binVersion) {
    const st = new ShardingTest({
        mongos: 1,
        config: 2,
        shards: 2,
        other: {
            mongosOptions: {binVersion: binVersion},
            configOptions: {binVersion: binVersion},
            rsOptions: {
                binVersion: binVersion,
            },
            rs: {nodes: 2},
        },
        // By default, our test infrastructure sets the election timeout to a very high value (24
        // hours). For this test, we need a shorter election timeout because it relies on nodes
        // running an election when they do not detect an active primary. Therefore, we are setting
        // the electionTimeoutMillis to its default value.
        initiateWithDefaultElectionTimeout: true
    });
    st.configRS.awaitReplication();

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

    return st;
}

function getNodeName(node) {
    const info = node.adminCommand({hello: 1});
    return info.setName + '_' + (info.secondary ? 'secondary' : 'primary');
}

function checkConfigAndShardsFCV(expectedFCV) {
    const configPrimary = st.configRS.getPrimary();

    const shard0Primary = st.rs0.getPrimary();
    const shard0Secondary = st.rs0.getSecondary();
    shard0Secondary.setSecondaryOk();

    const shard1Primary = st.rs1.getPrimary();
    const shard1Secondary = st.rs1.getSecondary();
    shard1Secondary.setSecondaryOk();

    for (const node
             of [configPrimary, shard0Primary, shard0Secondary, shard1Primary, shard1Secondary]) {
        jsTest.log('Verify that the FCV is properly set on node ' + getNodeName(node));

        const fcvDoc = node.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
        assert.eq(expectedFCV, fcvDoc.featureCompatibilityVersion.version);
    }
}

// TODO(SERVER-77873): Remove checkReshardingActiveIndex; once the feature flag is removed the
// check will be incorrect.
function checkReshardingActiveIndex() {
    const getActiveIndex = (node) => {
        const indexes = st.configRS.getPrimary().getDB("config").reshardingOperations.getIndexes();
        return indexes.find((index) => (index.name == "ReshardingCoordinatorActiveIndex"));
    };
    let activeIndex = getActiveIndex(st.configRS.getPrimary());
    if (FeatureFlagUtil.isPresentAndEnabled(st.s, "ReshardingImprovements")) {
        assert(
            !activeIndex,
            "With ReshardingImprovements enabled, the config.reshardingOperations ReshardingCoordinatorActiveIndex is present but should not be.");
    }
    // Since downgrading does not restore the index, we don't check for the index's presence
    // until we force a step-up (re-initializing the coordinator)

    st.configRS.awaitReplication();
    assert.commandWorked(st.configRS.getSecondary().adminCommand({replSetStepUp: 1}));
    st.configRS.waitForPrimaryOnlyServices(st.configRS.getPrimary());
    activeIndex = getActiveIndex(st.configRS.getPrimary());
    if (FeatureFlagUtil.isPresentAndEnabled(st.s, "ReshardingImprovements")) {
        assert(
            !activeIndex,
            "With ReshardingImprovements enabled, the config.reshardingOperations ReshardingCoordinatorActiveIndex is present but should not be, after step-up.");
    } else {
        assert(
            activeIndex,
            "With ReshardingImprovements disabled, the config.reshardingOperations ReshardingCoordinatorActiveIndex is not present but should be, after step-up.");
        assert(activeIndex.unique,
               "The config.reshardingOperations ReshardingCoordinatorActiveIndex is not unique");
    }
}

// TODO (SERVER-83264): Remove once 8.0 becomes last LTS.
function checkConfigSettingsSchema() {
    const configSettingsCollection = st.s.getDB("config").getCollection("settings");

    if (FeatureFlagUtil.isPresentAndEnabled(st.configRS.getPrimary(), "BalancerSettingsSchema")) {
        // chunksize schema should be enforced on both fcvs
        assert.commandWorked(configSettingsCollection.update(
            {_id: "chunksize"}, {$set: {value: 5}}, {upsert: true}));
        assert.commandFailed(configSettingsCollection.update(
            {_id: "chunksize"}, {$set: {value: -1}}, {upsert: true}));
        // After upgrade, the balancer settings schema should be enforced.
        assert.commandWorked(configSettingsCollection.update(
            {_id: "balancer"}, {_id: "balancer", mode: "full"}, {upsert: true}));
        assert.commandFailed(configSettingsCollection.update(
            {_id: "balancer"}, {$set: {stopped: "bad"}}, {upsert: true}));
    } else {
        // chunksize schema should be enforced on both fcvs
        assert.commandWorked(configSettingsCollection.update(
            {_id: "chunksize"}, {$set: {value: 5}}, {upsert: true}));
        assert.commandFailed(configSettingsCollection.update(
            {_id: "chunksize"}, {$set: {value: -1}}, {upsert: true}));
        // After downgrade, there should be no enforcement on the balancer settings.
        assert.commandWorked(configSettingsCollection.update(
            {_id: "balancer"}, {$set: {stopped: "bad"}}, {upsert: true}));

        // Set a valid value so the rest of the test finishes successfully.
        assert.commandWorked(configSettingsCollection.update(
            {_id: "balancer"}, {$set: {stopped: true}}, {upsert: true}));
    }
}

function checkClusterBeforeUpgrade(fcv) {
    // checkConfigSettingsSchema may not detect failures if there's a step-up. Keep as first check.
    checkConfigSettingsSchema();
    checkConfigAndShardsFCV(fcv);
    checkReshardingActiveIndex();
}

function checkClusterAfterBinaryUpgrade() {
}

function checkClusterAfterFCVUpgrade(fcv) {
    // checkConfigSettingsSchema may not detect failures if there's a step-up. Keep as first check.
    checkConfigSettingsSchema();
    checkConfigAndShardsFCV(fcv);
    checkReshardingActiveIndex();
}

function checkClusterAfterFCVDowngrade() {
    // checkConfigSettingsSchema may not detect failures if there's a step-up. Keep as first check.
    checkConfigSettingsSchema();
    checkReshardingActiveIndex();
}

function checkClusterAfterBinaryDowngrade(fcv) {
    checkConfigAndShardsFCV(fcv);
}

for (const oldVersion of [lastLTSFCV, lastContinuousFCV]) {
    //////////////////////////////
    // Setting and testing cluster using old binaries in default FCV mode

    jsTest.log('Deploying cluster version ' + oldVersion);
    var st = setupClusterAndDatabase(oldVersion);

    checkClusterBeforeUpgrade(oldVersion);

    //////////////////////////////
    // Setting and testing cluster using latest binaries in latest FCV mode

    jsTest.log('Upgrading binaries to latest version');
    st.upgradeCluster('latest');

    checkClusterAfterBinaryUpgrade();

    jsTest.log('Upgrading FCV to ' + latestFCV);
    assert.commandWorked(
        st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    checkClusterAfterFCVUpgrade(latestFCV);

    //////////////////////////////
    // Setting and testing cluster using old binaries in old FCV mode

    jsTest.log('Downgrading FCV to ' + oldVersion);
    assert.commandWorked(
        st.s.adminCommand({setFeatureCompatibilityVersion: oldVersion, confirm: true}));

    checkClusterAfterFCVDowngrade();

    jsTest.log('Downgrading binaries to version ' + oldVersion);
    st.downgradeCluster(oldVersion);

    checkClusterAfterBinaryDowngrade(oldVersion);

    st.stop();
}
