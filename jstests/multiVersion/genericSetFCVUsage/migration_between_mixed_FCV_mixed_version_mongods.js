/**
 * Test that it is not possible to move a chunk from an upgrade featureCompatibilityVersion node to
 * a downgrade binary version node.
 */

(function() {
"use strict";

// Test is not replSet so cannot clean up migration coordinator docs properly.
// Making it replSet will also make the moveChunk get stuck forever because the migration
// coordinator will retry forever and never succeeds because recipient shard has incompatible
// version.
TestData.skipCheckOrphans = true;

// Because in this test we explicitly leave shard1 in 'downgradeFCV' version, but the rest or the
// cluster has upgraded to 'latestFCV', shard1 won't be able to refresh its catalog cache from the
// upgraded configsvr due to incompatible wire version. This makes the index consistency check fail.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

// This test creates a sharded cluster with mixed binaries: one shard and all mongos are running an
// old binary whereas the config server and the other shards are running the lastest binary. The
// initial FCV of the sharded cluster is the older one. After some operations we explicitly advance
// the FCV to 'latest' to one of the shards that was running the new binary. Finally, we verify that
// we cannot move a chunk from a latestFCV + new binary shard to a downgraded binary version shard.
function runTest(downgradeVersion) {
    jsTestLog("Running test with downgradeVersion: " + downgradeVersion);

    let st = new ShardingTest({
        shards: [{binVersion: "latest"}, {binVersion: downgradeVersion}],
        mongos: 1,
        other: {
            shardAsReplicaSet: true,
            mongosOptions: {binVersion: downgradeVersion},
            configOptions: {binVersion: "latest"},
        }
    });

    const downgradeFCV = binVersionToFCV(downgradeVersion);
    checkFCV(st.configRS.getPrimary().getDB("admin"), downgradeFCV);
    checkFCV(st.shard0.getDB("admin"), downgradeFCV);
    checkFCV(st.shard1.getDB("admin"), downgradeFCV);

    // Create a sharded collection with primary shard 0.
    let testDB = st.s.getDB("test");
    assert.commandWorked(st.s.adminCommand({enableSharding: testDB.getName()}));
    st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);
    assert.commandWorked(
        st.s.adminCommand({shardCollection: testDB.coll.getFullName(), key: {a: 1}}));

    assert.commandWorked(st.shard0.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    checkFCV(st.configRS.getPrimary().getDB("admin"), downgradeFCV);
    checkFCV(st.shard0.getDB("admin"), latestFCV);
    checkFCV(st.shard1.getDB("admin"), downgradeFCV);

    // Invalid move chunk between shards with different binaries and FCVs. Pass explicit
    // writeConcern (which requires secondaryThrottle: true) to avoid problems if the downgraded
    // version doesn't automatically include writeConcern when running _recvChunkStart on the newer
    // shard.
    assert.commandFailedWithCode(st.s.adminCommand({
        moveChunk: testDB.coll.getFullName(),
        find: {a: 1},
        to: st.shard1.shardName,
        secondaryThrottle: true,
        writeConcern: {w: 1}
    }),
                                 ErrorCodes.IncompatibleServerVersion);

    st.stop();
}

runTest('last-lts');
})();
