/**
 * Tests that after setFeatureCompatibilityVersion=3.6, for all sharded collections, the UUID
 * reported by the shard in listCollections matches the UUID in config.collections.
 *
 * This should be true even if setFCV upgrade/downgrade is called repeatedly on the cluster, and
 * even if drops, recreates, and shardCollections are called in between the upgrades/downgrades.
 */

// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
    let st = new ShardingTest({shards: {rs0: {nodes: 1}}, other: {config: 3}});

    // Start in fcv=3.4.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.4"}));

    // We will manipulate collections across two databases.
    let db1 = "test1";
    let db2 = "test2";
    assert.commandWorked(st.s.adminCommand({enableSharding: db1}));
    st.ensurePrimaryShard(db1, st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({enableSharding: db2}));
    st.ensurePrimaryShard(db2, st.shard0.shardName);

    jsTest.log("shard some collections in each database");
    for (let i = 0; i < 3; i++) {
        let coll = "foo" + i;
        let nss1 = db1 + "." + coll;
        let nss2 = db2 + "." + coll;
        assert.commandWorked(st.s.adminCommand({shardCollection: nss1, key: {_id: 1}}));
        assert.commandWorked(st.s.adminCommand({shardCollection: nss2, key: {_id: 1}}));
    }

    jsTest.log("upgrade the cluster to fcv=3.6");
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.6"}));

    st.checkUUIDsConsistentAcrossCluster();

    // Drop some collections, shard some new collections, and drop and recreate some of the
    // collections as sharded with the same name.
    assert.commandWorked(st.s.getDB(db1).runCommand({drop: "foo0"}));
    assert.commandWorked(st.s.adminCommand({shardCollection: db1 + ".bar0", key: {_id: 1}}));
    assert.commandWorked(st.s.getDB(db1).runCommand({drop: "foo1"}));
    assert.commandWorked(st.s.adminCommand({shardCollection: db1 + ".foo1", key: {_id: 1}}));

    st.checkUUIDsConsistentAcrossCluster();

    jsTest.log("downgrade the cluster to fcv=3.4");
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.4"}));

    // Drop, recreate, and shard some collections again, now while downgraded.
    assert.commandWorked(st.s.getDB(db2).runCommand({drop: "foo0"}));
    assert.commandWorked(st.s.adminCommand({shardCollection: db2 + ".bar0", key: {_id: 1}}));
    assert.commandWorked(st.s.getDB(db2).runCommand({drop: "foo1"}));
    assert.commandWorked(st.s.adminCommand({shardCollection: db2 + ".foo1", key: {_id: 1}}));

    // We do not check UUID consistency after downgrading back to fcv=3.4, because the UUIDs are
    // deleted from shards on downgrade, but not from the config server's metadata.

    jsTest.log("re-upgrade the cluster to fcv=3.6");
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.6"}));

    st.checkUUIDsConsistentAcrossCluster();

    st.stop();
})();
