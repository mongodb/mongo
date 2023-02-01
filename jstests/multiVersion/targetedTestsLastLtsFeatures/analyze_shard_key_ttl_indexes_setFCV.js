/*
 * Tests that version upgrade creates the TTL indexes for config.sampledQueries and
 * config.sampledQueriesDiff.
 *
 * @tags: [featureFlagAnalyzeShardKey]
 */

(function() {
"use strict";

load('./jstests/multiVersion/libs/multi_rs.js');
load('./jstests/multiVersion/libs/multi_cluster.js');
load("./jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

/**
 * Verifies that a proper TTL index exists for the query sample collection
 */
function assertTTLIndexExists(node, collName, indexName) {
    const configDB = node.getDB("config");
    let foundIndexSpec = undefined;
    assert.soon(() => {
        const indexSpecs =
            assert.commandWorked(configDB.runCommand({"listIndexes": collName})).cursor.firstBatch;
        for (var i = 0; i < indexSpecs.length; ++i) {
            if (indexSpecs[i].name == indexName) {
                foundIndexSpec = indexSpecs[i];
                return true;
            }
        }
        return false;
    });
    assert.eq(foundIndexSpec.key, {"expireAt": 1});
    assert.eq(foundIndexSpec.expireAfterSeconds, 0);
}

function assertTTLIndexesExist(node) {
    assertTTLIndexExists(node, "sampledQueries", "SampledQueriesTTLIndex");
    assertTTLIndexExists(node, "sampledQueriesDiff", "SampledQueriesDiffTTLIndex");
}

for (let oldVersion of ["last-lts", "last-continuous"]) {
    jsTest.log("Start testing with version " + oldVersion);
    var st = new ShardingTest({
        shards: 1,
        rs: {nodes: 2},
        mongos: 1,
        other: {
            mongosOptions: {binVersion: oldVersion},
            configOptions: {binVersion: oldVersion},
            shardOptions: {binVersion: oldVersion},
            rsOptions: {binVersion: oldVersion}
        }
    });
    st.configRS.awaitReplication();

    //////// Upgrade to latest

    // Upgrade the config servers
    jsTest.log('upgrading config servers');
    st.upgradeCluster("latest", {upgradeMongos: false, upgradeShards: false});
    // Restart mongos to clear all cache and force it to do remote calls.
    st.restartMongoses();

    // Upgrade the shards
    jsTest.log('upgrading shard servers');
    st.upgradeCluster("latest", {upgradeMongos: false, upgradeConfigs: false});
    awaitRSClientHosts(st.s, st.rs0.getPrimary(), {ok: true, ismaster: true});
    // Restart mongos to clear all cache and force it to do remote calls.
    st.restartMongoses();

    assertTTLIndexesExist(st.rs0.getPrimary());

    // Upgrade mongos
    jsTest.log('upgrading mongos servers');
    st.upgradeCluster("latest", {upgradeConfigs: false, upgradeShards: false});
    // Restart mongos to clear all cache and force it to do remote calls.
    st.restartMongoses();

    assertTTLIndexesExist(st.rs0.getPrimary());

    // Check that version document is unmodified.
    version = st.s.getCollection('config.version').findOne();
    var clusterID = version.clusterId;
    assert.eq(clusterID, version.clusterId);

    //////// Downgrade back

    jsTest.log('downgrading mongos servers');
    st.downgradeCluster(oldVersion, {downgradeConfigs: false, downgradeShards: false});
    // Restart mongos to clear all cache and force it to do remote calls.
    st.restartMongoses();

    assertTTLIndexesExist(st.rs0.getPrimary());

    jsTest.log('downgrading shard servers');
    st.downgradeCluster(oldVersion, {downgradeMongos: false, downgradeConfigs: false});
    awaitRSClientHosts(st.s, st.rs0.getPrimary(), {ok: true, ismaster: true});
    // Restart mongos to clear all cache and force it to do remote calls.
    st.restartMongoses();

    for (let conn of [st.rs0.getPrimary(), st.rs0.getSecondary()]) {
        assertTTLIndexesExist(conn);
    }

    jsTest.log('downgrading config servers');
    st.downgradeCluster(oldVersion, {downgradeMongos: false, downgradeShards: false});
    // Restart mongos to clear all cache and force it to do remote calls.
    st.restartMongoses();

    // Check that version document is unmodified.
    version = st.s.getCollection('config.version').findOne();
    assert.eq(clusterID, version.clusterId);

    jsTest.log("End testing with version " + oldVersion);
    st.stop();
}
})();
