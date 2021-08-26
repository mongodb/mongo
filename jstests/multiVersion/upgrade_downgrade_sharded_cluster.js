/**
 * Verify that some metadata is properly changed after the upgrade and downgrade of a sharded
 * cluster. More specifically:
 *
 *	1. Create a sharded cluster in replica set running an old binary version
 *	2. Setup some data on cluster
 *	3. Upgrade binaries and FCV of the cluster to the latest version
 *	4. Verify the data consistency after the upgrade procedure
 *  5. Downgrade binaries and FCV of the cluster to an old version
 *	6. Verify the data consistency after the downgrade procedure
 */
(function() {
'use strict';

load('jstests/libs/uuid_util.js');                   // For extractUUIDFromObject
load('jstests/multiVersion/libs/multi_cluster.js');  // For upgradeCluster

function setupClusterAndDatabase(binVersion) {
    const st = new ShardingTest({
        mongos: 1,
        config: 1,
        shards: 2,
        other: {
            mongosOptions: {binVersion: binVersion},
            configOptions: {binVersion: binVersion},
            shardOptions: {binVersion: binVersion},
            rs: {nodes: 2}
        }
    });
    st.configRS.awaitReplication();

    assert.commandWorked(
        st.s.adminCommand({enableSharding: jsTestName(), primaryShard: st.shard0.shardName}));

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

var collIndex = 1;
function createShardedCollection() {
    const collNs = jsTestName() + '.sharded_coll' + collIndex++;
    assert.commandWorked(st.s.adminCommand({shardCollection: collNs, key: {x: 1}}));

    const coll = st.s.getCollection(collNs);
    assert.commandWorked(coll.insert({x: -1}));
    assert.commandWorked(coll.insert({x: 1}));

    assert.commandWorked(st.s.adminCommand({split: collNs, middle: {x: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: collNs, find: {x: 1}, to: st.shard1.shardName}));

    return collNs;
}

function testDisabledLongNameSupport(collNs) {
    jsTestLog('Verify that long name support is properly disabled on collection ' + collNs);

    const collConfigDoc = st.s.getDB('config').collections.findOne({_id: collNs});
    assert.eq(collConfigDoc.supportingLongName, undefined);

    const shard0Primary = st.rs0.getPrimary();
    const shard0Secondary = st.rs0.getSecondary();
    shard0Secondary.setSecondaryOk();

    const shard1Primary = st.rs1.getPrimary();
    const shard1Secondary = st.rs1.getSecondary();
    shard1Secondary.setSecondaryOk();

    for (const node of [shard0Primary, shard0Secondary, shard1Primary, shard1Secondary]) {
        jsTestLog('Verify the consistency of the persisted cache on node ' + getNodeName(node));

        const configDb = node.getDB('config');

        const cachedCollDoc = configDb['cache.collections'].findOne({_id: collNs});
        assert.neq(cachedCollDoc, null);

        assert(configDb['cache.chunks.' + collNs].exists());
        assert(!configDb['cache.chunks.' + extractUUIDFromObject(cachedCollDoc.uuid)].exists());
    }
}

function testImplicitlyEnabledLongNameSupport(collNs) {
    jsTestLog('Verify that long name support is properly enabled on collection ' + collNs);

    const collConfigDoc = st.s.getDB('config').collections.findOne({_id: collNs});
    assert.eq(collConfigDoc.supportingLongName, 'implicitly_enabled');

    const shard0Primary = st.rs0.getPrimary();
    const shard0Secondary = st.rs0.getSecondary();
    shard0Secondary.setSecondaryOk();

    const shard1Primary = st.rs1.getPrimary();
    const shard1Secondary = st.rs1.getSecondary();
    shard1Secondary.setSecondaryOk();

    for (const node of [shard0Primary, shard0Secondary, shard1Primary, shard1Secondary]) {
        jsTestLog('Verify the consistency of the persisted cache on node ' + getNodeName(node));

        const configDb = node.getDB('config');

        const cachedCollDoc = configDb['cache.collections'].findOne({_id: collNs});
        assert.neq(cachedCollDoc, null);

        assert(!configDb['cache.chunks.' + collNs].exists());
        assert(configDb['cache.chunks.' + extractUUIDFromObject(cachedCollDoc.uuid)].exists());
    }
}

function checkClusterBeforeUpgrade(fcv, collNs) {
    checkConfigAndShardsFCV(fcv);
    testDisabledLongNameSupport(collNs);
}

function checkClusterAfterBinaryUpgrade() {
    // To implement in the future, if necessary.
}

function checkClusterAfterFCVUpgrade(fcv, call1Ns, call2Ns) {
    checkConfigAndShardsFCV(fcv);
    testImplicitlyEnabledLongNameSupport(call1Ns);
    testImplicitlyEnabledLongNameSupport(call2Ns);
}

function checkClusterAfterFCVDowngrade() {
    // To implement in the future, if necessary.
}

function checkClusterAfterBinaryDowngrade(fcv, call1Ns, call2Ns, call3Ns) {
    checkConfigAndShardsFCV(fcv);
    testDisabledLongNameSupport(call1Ns);
    testDisabledLongNameSupport(call2Ns);
    testDisabledLongNameSupport(call3Ns);
}

for (const oldVersion of [lastLTSFCV, lastContinuousFCV]) {
    //////////////////////////////
    // Setting and testing cluster using old binaries in default FCV mode

    jsTest.log('Deploying cluster version ' + oldVersion);
    var st = setupClusterAndDatabase(oldVersion);

    const collCreatedBeforeClusterUpgrade = createShardedCollection();
    checkClusterBeforeUpgrade(oldVersion, collCreatedBeforeClusterUpgrade);

    //////////////////////////////
    // Setting and testing cluster using latest binaries in latest FCV mode

    jsTest.log('Upgrading binaries to latest version');
    st.upgradeCluster('latest');

    checkClusterAfterBinaryUpgrade();

    jsTest.log('Upgrading FCV to ' + latestFCV);
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    const collCreatedAfterClusterUpgrade = createShardedCollection();
    checkClusterAfterFCVUpgrade(
        latestFCV, collCreatedBeforeClusterUpgrade, collCreatedAfterClusterUpgrade);

    //////////////////////////////
    // Setting and testing cluster using old binaries in old FCV mode

    jsTest.log('Downgrading FCV to ' + oldVersion);
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: oldVersion}));

    checkClusterAfterFCVDowngrade();

    jsTest.log('Downgrading binaries to version ' + oldVersion);
    st.upgradeCluster(oldVersion);

    const collCreatedAfterClusterDowngrade = createShardedCollection();
    checkClusterAfterBinaryDowngrade(oldVersion,
                                     collCreatedBeforeClusterUpgrade,
                                     collCreatedAfterClusterUpgrade,
                                     collCreatedAfterClusterDowngrade);

    st.stop();
}
})();
