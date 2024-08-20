/**
 * Tests that CRUD, aggregation and DDL commands continue to work as expected with a config shard on
 * both sharded and unsharded collection at each step of cluster upgrade/downgrade between last-lts
 * and latest and between last-continuous and latest.
 */
import "jstests/multiVersion/libs/multi_rs.js";
import "jstests/multiVersion/libs/multi_cluster.js";

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    testCRUDAndAgg,
    testDDLOps
} from "jstests/multiVersion/libs/upgrade_downgrade_cluster_shared.js";
import {awaitRSClientHosts} from "jstests/replsets/rslib.js";

// When checking UUID consistency, the shell attempts to run a command on the node it believes is
// primary in each shard. However, this test restarts shards, and the node that is elected primary
// after the restart may be different from the original primary. Since the shell does not retry on
// NotWritablePrimary errors, and whether or not it detects the new primary before issuing the
// command is nondeterministic, skip the consistency check for this test.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

// Test upgrade/downgrade between "latest" and "last-lts"/"last-continuous".
for (let oldVersion of ["last-lts", "last-continuous"]) {
    var st = new ShardingTest({
        shards: 2,
        mongos: 1,
        other: {
            mongosOptions: {binVersion: oldVersion},
            configOptions: {binVersion: oldVersion},
            shardOptions: {binVersion: oldVersion},

            rsOptions: {binVersion: oldVersion},
            rs: true,
        },
        configShard: true
    });
    st.configRS.awaitReplication();

    // check that config.version document gets initialized properly
    var version = st.s.getCollection('config.version').findOne();
    var clusterID = version.clusterId;
    assert.neq(null, clusterID);

    // Setup sharded collection
    assert.commandWorked(
        st.s.adminCommand({enableSharding: 'sharded', primaryShard: st.shard0.shardName}));

    assert.commandWorked(st.s.adminCommand({shardCollection: 'sharded.foo', key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: 'sharded.foo', middle: {x: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: 'sharded.foo', find: {x: 1}, to: st.shard1.shardName}));

    testCRUDAndAgg(st.s.getDB('unsharded'));
    testCRUDAndAgg(st.s.getDB('sharded'));
    testDDLOps(st);

    const testCluster = () => {
        testCRUDAndAgg(st.s.getDB('unsharded'));
        testCRUDAndAgg(st.s.getDB('sharded'));
        testDDLOps(st);

        // Restart mongos to clear all cache and force it to do remote calls.
        st.restartMongoses();

        testCRUDAndAgg(st.s.getDB('unsharded'));
        testCRUDAndAgg(st.s.getDB('sharded'));
        testDDLOps(st);
    };

    // upgrade the config servers first
    jsTest.log('upgrading config servers (back-compat)');
    st.upgradeBinariesWithBackCompat("latest", {upgradeMongos: false, upgradeShards: false});

    testCluster();

    // Then upgrade the shards.
    jsTest.log('upgrading shard servers (back-compat)');
    st.upgradeBinariesWithBackCompat("latest", {upgradeMongos: false, upgradeConfigs: false});

    awaitRSClientHosts(st.s, st.rs0.getPrimary(), {ok: true, ismaster: true});
    awaitRSClientHosts(st.s, st.rs1.getPrimary(), {ok: true, ismaster: true});

    testCluster();

    // Finally, upgrade mongos
    jsTest.log('upgrading mongos servers (back-compat)');
    st.upgradeBinariesWithBackCompat("latest", {upgradeConfigs: false, upgradeShards: false});

    testCluster();

    // Repeat everything without back-compat

    // upgrade the config servers first
    jsTest.log('upgrading config servers (no back-compat)');
    st.restartBinariesWithoutUpgradeBackCompat("latest",
                                               {upgradeMongos: false, upgradeShards: false});

    testCluster();

    // Then upgrade the shards.
    jsTest.log('upgrading shard servers (no back-compat)');
    st.restartBinariesWithoutUpgradeBackCompat("latest",
                                               {upgradeMongos: false, upgradeConfigs: false});

    awaitRSClientHosts(st.s, st.rs0.getPrimary(), {ok: true, ismaster: true});
    awaitRSClientHosts(st.s, st.rs1.getPrimary(), {ok: true, ismaster: true});

    testCluster();

    // Finally, upgrade mongos
    jsTest.log('upgrading mongos servers (no back-compat)');
    st.restartBinariesWithoutUpgradeBackCompat("latest",
                                               {upgradeConfigs: false, upgradeShards: false});

    testCluster();

    // Check that version document is unmodified.
    version = st.s.getCollection('config.version').findOne();
    assert.eq(clusterID, version.clusterId);

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Downgrade back

    jsTest.log('downgrading mongos servers (back-compat)');
    st.restartBinariesWithDowngradeBackCompat("latest",
                                              {downgradeConfigs: false, downgradeShards: false});

    testCluster();

    jsTest.log('downgrading shard servers (back-compat)');
    st.restartBinariesWithDowngradeBackCompat("latest",
                                              {downgradeMongos: false, downgradeConfigs: false});

    awaitRSClientHosts(st.s, st.rs0.getPrimary(), {ok: true, ismaster: true});
    awaitRSClientHosts(st.s, st.rs1.getPrimary(), {ok: true, ismaster: true});

    testCluster();

    jsTest.log('downgrading config servers (back-compat)');
    st.restartBinariesWithDowngradeBackCompat("latest",
                                              {downgradeMongos: false, downgradeShards: false});

    testCluster();

    // Repeat everything without back-compat

    jsTest.log('downgrading mongos servers (no back-compat)');
    st.downgradeBinariesWithoutDowngradeBackCompat(
        oldVersion, {downgradeConfigs: false, downgradeShards: false});

    testCluster();

    jsTest.log('downgrading shard servers (no back-compat)');
    st.downgradeBinariesWithoutDowngradeBackCompat(
        oldVersion, {downgradeMongos: false, downgradeConfigs: false});

    awaitRSClientHosts(st.s, st.rs0.getPrimary(), {ok: true, ismaster: true});
    awaitRSClientHosts(st.s, st.rs1.getPrimary(), {ok: true, ismaster: true});

    testCluster();

    jsTest.log('downgrading config servers (no back-compat)');
    st.downgradeBinariesWithoutDowngradeBackCompat(
        oldVersion, {downgradeMongos: false, downgradeShards: false});

    testCluster();

    // Check that version document is unmodified.
    version = st.s.getCollection('config.version').findOne();
    assert.eq(clusterID, version.clusterId);

    st.stop();
}
