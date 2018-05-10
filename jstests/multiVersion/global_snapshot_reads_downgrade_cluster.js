/**
 * Test the downgrade of a sharded cluster from latest to last-stable version succeeds, verifying
 * the behavior of global snapshot reads throughout the process.
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

    // Start a cluster with two shards and two mongos at the latest version.
    var st = new ShardingTest({
        shards: 2,
        mongos: 2,
        other: {
            mongosOptions: {binVersion: "latest"},
            configOptions: {binVersion: "latest"},
            rsOptions: {binVersion: "latest"},
        },
        rs: {nodes: 3}  // Use 3 node replica sets to allow downgrades with no downtime.
    });
    checkFCV(st.configRS.getPrimary().getDB("admin"), latestFCV);

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

    // Global snapshot reads are accepted.
    verifyGlobalSnapshotReads(st.s0, true);
    verifyGlobalSnapshotReads(st.s1, true);

    // Downgrade featureCompatibilityVersion to 3.6.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(st.configRS.getPrimary().getDB("admin"), lastStableFCV);

    // Global snapshot reads are accepted.
    verifyGlobalSnapshotReads(st.s0, true);
    verifyGlobalSnapshotReads(st.s1, true);

    // Downgrade the mongos servers first.
    jsTest.log("Downgrading mongos servers.");
    st.upgradeCluster("last-stable",
                      {upgradeConfigs: false, upgradeMongos: true, upgradeShards: false});

    // Global snapshot reads are rejected with InvalidOptions, because downgraded mongos will not
    // forward the txnNumber to the shards.
    verifyGlobalSnapshotReads(st.s0, false, ErrorCodes.InvalidOptions);
    verifyGlobalSnapshotReads(st.s1, false, ErrorCodes.InvalidOptions);

    // Then downgrade the shard servers.
    jsTest.log("Downgrading shard servers.");
    st.upgradeCluster("last-stable",
                      {upgradeConfigs: false, upgradeMongos: false, upgradeShards: true});

    // Global snapshot reads are rejected with FailedToParse, because the shards will reject the
    // unknown readConcern field.
    verifyGlobalSnapshotReads(st.s0, false, ErrorCodes.FailedToParse);
    verifyGlobalSnapshotReads(st.s1, false, ErrorCodes.FailedToParse);

    // Finally, downgrade the config servers.
    jsTest.log("Downgrading config servers.");
    st.upgradeCluster("last-stable",
                      {upgradeConfigs: true, upgradeMongos: false, upgradeShards: false});

    // Global snapshot reads are rejected with FailedToParse, because the shards will reject the
    // unknown readConcern field.
    verifyGlobalSnapshotReads(st.s0, false, ErrorCodes.FailedToParse);
    verifyGlobalSnapshotReads(st.s1, false, ErrorCodes.FailedToParse);

    st.stop();
})();
