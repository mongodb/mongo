/**
 * Test that it is not possible to move a chunk from an upgrade featureCompatibilityVersion node to
 * a downgrade binary version node.
 */

// This test will not end with consistent UUIDs, since there is inconsistent
// featureCompatibilityVersion across the cluster.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
    "use strict";

    load("jstests/libs/feature_compatibility_version.js");

    const upgradeVersion = "3.6";
    const downgradeVersion = "3.4";

    let st = new ShardingTest({
        shards: [{binVersion: "latest"}, {binVersion: downgradeVersion}],
        mongos: {binVersion: "latest"}
    });

    let testDB = st.s.getDB("test");

    // Create a sharded collection with primary shard 0.
    assert.commandWorked(st.s.adminCommand({enableSharding: testDB.getName()}));
    st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);
    assert.commandWorked(
        st.s.adminCommand({shardCollection: testDB.coll.getFullName(), key: {a: 1}}));

    // Set the featureCompatibilityVersion to 3.6. This will fail because the
    // featureCompatibilityVersion cannot be set to 3.6 on shard 1, but it will set the
    // featureCompatibilityVersion to 3.6 on shard 0.
    assert.commandFailed(st.s.adminCommand({setFeatureCompatibilityVersion: upgradeVersion}));
    checkFCV(st.configRS.getPrimary().getDB("admin"), downgradeVersion, upgradeVersion);
    checkFCV(st.shard0.getDB("admin"), upgradeVersion);
    checkFCV34(st.shard1.getDB("admin"), downgradeVersion);

    // It is not possible to move a chunk from an upgrade featureCompatibilityVersion shard to a
    // downgrade shard.
    assert.commandFailedWithCode(
        st.s.adminCommand(
            {moveChunk: testDB.coll.getFullName(), find: {a: 1}, to: st.shard1.shardName}),
        ErrorCodes.IncompatibleServerVersion);

    st.stop();
})();
