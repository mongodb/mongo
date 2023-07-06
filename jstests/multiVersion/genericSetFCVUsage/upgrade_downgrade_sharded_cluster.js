import "jstests/multiVersion/libs/multi_cluster.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

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
        }
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

function checkClusterBeforeUpgrade(fcv) {
    checkConfigAndShardsFCV(fcv);
    checkReshardingActiveIndex();
}

function checkClusterAfterBinaryUpgrade() {
}

function checkClusterAfterFCVUpgrade(fcv) {
    checkConfigAndShardsFCV(fcv);
    checkReshardingActiveIndex();
}

function checkClusterAfterFCVDowngrade() {
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
