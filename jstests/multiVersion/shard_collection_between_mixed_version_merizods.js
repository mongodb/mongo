//
// Testing shardCollection between 4.0.1 and latest merizod versions for both config servers
// and shards.
//

load("./jstests/multiVersion/libs/verify_versions.js");

(function() {
    "use strict";

    var options = {
        shards: [{binVersion: "latest"}, {binVersion: "4.0.1"}, {binVersion: "4.0.1"}],
        merizos: 1,
        other: {
            merizosOptions: {binVersion: "latest"},
            configOptions: {binVersion: "latest"},
            shardAsReplicaSet: true
        }
    };

    var st = new ShardingTest(options);
    assert.binVersion(st.shard0, "latest");
    assert.binVersion(st.shard1, "4.0.1");
    assert.binVersion(st.shard2, "4.0.1");
    assert.binVersion(st.s0, "latest");

    var merizos = st.s0;
    var admin = merizos.getDB('admin');

    const kDBOnShardWithLatestBinary = "DBWithPrimaryOnLatestBinary";
    const kNSOnLatestShard = kDBOnShardWithLatestBinary + ".Coll";
    const kDBOnShardWithOldBinary = "DBWithPrimaryOnOldBinary";
    const kNSOnOldShard = kDBOnShardWithOldBinary + ".Coll";

    assert.commandWorked(admin.runCommand({enableSharding: kDBOnShardWithLatestBinary}));
    assert.commandWorked(admin.runCommand({enableSharding: kDBOnShardWithOldBinary}));
    st.ensurePrimaryShard(kDBOnShardWithLatestBinary, st.shard0.shardName);
    st.ensurePrimaryShard(kDBOnShardWithOldBinary, st.shard1.shardName);

    // Test that shardCollection succeeds when both the config server and primary shard are
    // running with latest binVersion, but other shards are running with 4.0.1 which does not
    // have the new shardCollection protocol.
    assert.commandWorked(admin.runCommand({shardCollection: kNSOnLatestShard, key: {a: 1}}));

    // Test that shardCollection succeeds when the config server is running with the latest
    // binVersion, but the primary is running with 4.0.1.
    assert.commandWorked(admin.runCommand({shardCollection: kNSOnOldShard, key: {a: 1}}));

    merizos.getDB(kDBOnShardWithLatestBinary).Coll.drop();
    merizos.getDB(kDBOnShardWithOldBinary).Coll.drop();

    // Test that shardCollection with a hashed shard key succeeds when both the config server and
    // primary shard are running with latest binVersion, but other shards are running with 4.0.1
    // which does not have the new shardCollection protocol.
    assert.commandWorked(admin.runCommand({shardCollection: kNSOnLatestShard, key: {a: "hashed"}}));

    // Test that shardCollection with a hashed shard key succeeds when the config server is running
    // with the latest binVersion, but the primary is running with 4.0.1.
    assert.commandWorked(admin.runCommand({shardCollection: kNSOnOldShard, key: {a: "hashed"}}));

    st.stop();
})();
