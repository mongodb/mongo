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

(function() {
'use strict';

load('jstests/multiVersion/libs/multi_cluster.js');  // For upgradeCluster

const dbName = jsTestName();

// TODO SERVER-57417 remove feature flag check once enabled
const orphansTrackingFeatureFlagEnabled = TestData.setParameters.featureFlagOrphanTracking;

//==========//
// TODO SERVER-64400 remove code delimited with //==========// once 6.0 branches out
const kRangeDeletionNs = "config.rangeDeletions";
const testOrphansTrackingNS = dbName + '.testOrphansTracking';
const numOrphanedDocs = 10;
if (orphansTrackingFeatureFlagEnabled) {
    TestData.skipCheckOrphans = true;
}
//==========//

function setupClusterAndDatabase(binVersion) {
    // TODO SERVER-64400 remove params related with no-more-autosplitter once 6.0 branches out
    const params = orphansTrackingFeatureFlagEnabled ? {disableResumableRangeDeleter: true} : {};
    const st = new ShardingTest({
        mongos: 1,
        config: 1,
        shards: 2,
        other: {
            mongosOptions: {binVersion: binVersion},
            configOptions: {binVersion: binVersion},
            rsOptions: {
                binVersion: binVersion,
                setParameter: params,
            },
            rs: {nodes: 2},
            enableBalancer: orphansTrackingFeatureFlagEnabled ? false : true
        }
    });
    st.configRS.awaitReplication();

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

    // TODO SERVER-57417 remove feature flag check once enabled
    if (orphansTrackingFeatureFlagEnabled) {
        // TODO SERVER-64400 remove this scope once 6.0 branches out
        // - Shard collection (one big chunk on shard0)
        // - Insert data in range [0, MaxKey)
        // - Split chunk at 0
        // - Move chunks [0, MaxKey] on shard1
        assert.commandWorked(
            st.s.adminCommand({shardCollection: testOrphansTrackingNS, key: {_id: 1}}));
        var batch = st.s.getCollection(testOrphansTrackingNS).initializeOrderedBulkOp();
        for (var i = 0; i < numOrphanedDocs; i++) {
            batch.insert({_id: i});
        }
        assert.commandWorked(batch.execute());
        assert.commandWorked(st.splitAt(testOrphansTrackingNS, {_id: 0}));
        st.s.adminCommand(
            {moveChunk: testOrphansTrackingNS, find: {_id: 0}, to: st.shard1.shardName});
    }

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

function checkClusterBeforeUpgrade(fcv) {
    checkConfigAndShardsFCV(fcv);
}

function checkClusterAfterBinaryUpgrade() {
    // To implement in the future, if necessary.
}

function checkClusterAfterFCVUpgrade(fcv) {
    checkConfigAndShardsFCV(fcv);
    // TODO SERVER-57417 remove feature flag check once enabled
    if (orphansTrackingFeatureFlagEnabled) {
        // TODO SERVER-64400 remove this scope once 6.0 branches out
        // Check that orphans counter has been populated
        var doc = st.shard0.getCollection(kRangeDeletionNs).findOne({nss: testOrphansTrackingNS});
        assert.eq(numOrphanedDocs, doc.numOrphanDocs);
    }
}

function checkClusterAfterFCVDowngrade() {
    // TODO SERVER-57417 remove feature flag check once enabled
    if (orphansTrackingFeatureFlagEnabled) {
        // TODO SERVER-64400 remove this scope once 6.0 branches out
        // Check that orphans counter has been unset
        var doc = st.shard0.getCollection(kRangeDeletionNs).findOne({nss: testOrphansTrackingNS});
        assert.eq(undefined, doc.numOrphanDocs);
    }
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
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    checkClusterAfterFCVUpgrade(latestFCV);

    //////////////////////////////
    // Setting and testing cluster using old binaries in old FCV mode

    jsTest.log('Downgrading FCV to ' + oldVersion);
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: oldVersion}));

    checkClusterAfterFCVDowngrade();

    jsTest.log('Downgrading binaries to version ' + oldVersion);
    st.downgradeCluster(oldVersion);

    checkClusterAfterBinaryDowngrade(oldVersion);

    st.stop();
}
})();
