/**
 * Tests upgrading a cluster with two shards and two mongos servers from last stable to current
 * version, verifying the behavior of global snapshot reads throughout the process.
 */

// Checking UUID consistency uses cached connections, which are not valid across restarts or
// stepdowns.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
    "use strict";

    load("jstests/libs/feature_compatibility_version.js");
    load("jstests/multiVersion/libs/multi_rs.js");
    load("jstests/multiVersion/libs/multi_cluster.js");
    load("jstests/multiVersion/libs/global_snapshot_reads_helpers.js");

    if (!supportsSnapshotReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support snapshot read concern.");
        return;
    }

    // Start a cluster with two shards and two mongos at the last stable version.
    var st = new ShardingTest({
        shards: 2,
        mongos: 2,
        other: {
            configOptions: {binVersion: "last-stable"},
            mongosOptions: {binVersion: "last-stable"},
            rsOptions: {binVersion: "last-stable"},
        },
        rs: {nodes: 3}  // Use 3 node replica sets to allow upgrades with no downtime.
    });

    // Setup a sharded collection with two chunks, one on each shard.
    assert.commandWorked(st.s.adminCommand({enableSharding: "shardedDb"}));
    st.ensurePrimaryShard("shardedDb", st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({shardCollection: "shardedDb.sharded", key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: "shardedDb.sharded", middle: {x: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: "shardedDb.sharded", find: {x: 1}, to: st.shard1.shardName}));

    // Insert some data for the reads to find.
    st.s.getDB("unshardedDb").unsharded.insert({x: 1});
    st.s.getDB("shardedDb").sharded.insert([{x: -1}, {x: 1}]);

    // Global snapshot reads are rejected with FailedToParse, because the shards will reject the
    // unknown readConcern field.
    verifyGlobalSnapshotReads(st.s0, false, ErrorCodes.FailedToParse);
    verifyGlobalSnapshotReads(st.s1, false, ErrorCodes.FailedToParse);

    // Upgrade the config servers.
    jsTest.log("Upgrading config servers.");
    st.upgradeCluster("latest", {upgradeConfigs: true, upgradeMongos: false, upgradeShards: false});

    // Global snapshot reads are rejected with FailedToParse, because the shards will reject the
    // unknown readConcern field.
    verifyGlobalSnapshotReads(st.s0, false, ErrorCodes.FailedToParse);
    verifyGlobalSnapshotReads(st.s1, false, ErrorCodes.FailedToParse);

    // Then upgrade the shard servers.
    jsTest.log("Upgrading shard servers.");
    st.upgradeCluster("latest", {upgradeConfigs: false, upgradeMongos: false, upgradeShards: true});

    // Global snapshot reads are rejected with InvalidOptions, because mongos will not forward the
    // txnNumber to the upgraded shards.
    verifyGlobalSnapshotReads(st.s0, false, ErrorCodes.InvalidOptions);
    verifyGlobalSnapshotReads(st.s1, false, ErrorCodes.InvalidOptions);

    // Finally, upgrade mongos servers.
    jsTest.log("Upgrading mongos servers.");
    st.upgradeCluster("latest", {upgradeConfigs: false, upgradeMongos: true, upgradeShards: false});
    checkFCV(st.configRS.getPrimary().getDB("admin"), lastStableFCV);

    // Global snapshot reads are accepted.
    verifyGlobalSnapshotReads(st.s0, true);
    verifyGlobalSnapshotReads(st.s1, true);

    // Upgrade the cluster's feature compatibility version to the latest.
    assert.commandWorked(
        st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(st.configRS.getPrimary().getDB("admin"), latestFCV);

    // Global snapshot reads are accepted.
    verifyGlobalSnapshotReads(st.s0, true);
    verifyGlobalSnapshotReads(st.s1, true);

    st.stop();
})();
