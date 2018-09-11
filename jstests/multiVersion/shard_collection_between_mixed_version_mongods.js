//
// Testing shardCollection between 4.0.1 and latest mongod versions for both config servers
// and shards.
//

load("./jstests/multiVersion/libs/verify_versions.js");

(function() {
    "use strict";

    var options = {
        shards: [{binVersion: "latest"}, {binVersion: "4.0.1"}, {binVersion: "4.0.1"}],
        mongos: 1,
        other: {
            mongosOptions: {binVersion: "latest"},
            configOptions: {binVersion: "latest"},
            shardAsReplicaSet: true
        }
    };

    var st = new ShardingTest(options);
    st.stopBalancer();

    assert.binVersion(st.shard0, "latest");
    assert.binVersion(st.shard1, "4.0.1");
    assert.binVersion(st.shard2, "4.0.1");
    assert.binVersion(st.s0, "latest");

    var mongos = st.s0;
    var admin = mongos.getDB('admin');
    var shards = mongos.getCollection('config.shards').find().toArray();
    const fooDB = "fooTest";
    const fooNS = fooDB + ".foo";
    const barDB = "barTest";
    const barNS = barDB + ".foo";

    assert.commandWorked(admin.runCommand({enableSharding: fooDB}));
    assert.commandWorked(admin.runCommand({enableSharding: barDB}));
    st.ensurePrimaryShard(fooDB, shards[0]._id);
    st.ensurePrimaryShard(barDB, shards[1]._id);

    // Test that shardCollection succeeds when both the config server and primary shard are
    // running with latest binVersion, but other shards are running with 4.0.1 which does not
    // have the new shardCollection protocol.
    assert.commandWorked(admin.runCommand({shardCollection: fooNS, key: {a: 1}}));

    // Test that shardCollection succeeds when the config server is running with the latest
    // binVersion, but the primary is running with 4.0.1.
    assert.commandWorked(admin.runCommand({shardCollection: barNS, key: {a: 1}}));

    mongos.getDB(fooDB).foo.drop();
    mongos.getDB(barDB).foo.drop();

    // Test that shardCollection with a hashed shard key succeeds when both the config server and
    // primary shard are running with latest binVersion, but other shards are running with 4.0.1
    // which does not have the new shardCollection protocol.
    assert.commandWorked(admin.runCommand({shardCollection: fooNS, key: {a: "hashed"}}));

    // Test that shardCollection with a hashed shard key succeeds when the config server is running
    // with the latest binVersion, but the primary is running with 4.0.1.
    assert.commandWorked(admin.runCommand({shardCollection: barNS, key: {a: "hashed"}}));

    st.stop();
})();