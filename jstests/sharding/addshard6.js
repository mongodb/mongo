/**
 * Test that adding a config server replica set as a shard fails.
 */
(function() {

    var addShardRes;

    // Note: this method expects that the failure is *not* that the specified shardName is already
    // the shardName of an existing shard.
    var assertAddShardFailed = function(res, shardName) {
        assert.commandFailed(res);

        // If a shard name was specified in the addShard, make sure no shard with its name shows up
        // in config.shards.
        if (shardName) {
            assert.eq(null,
                      st.s.getDB('config').shards.findOne({_id: shardName}),
                      "addShard for " + shardName +
                          " reported failure, but shard shows up in config.shards");
        }
    };

    var st = new ShardingTest({
        shards: 0,
        mongos: 1,
    });

    var configRS = new ReplSetTest({name: "configsvrReplicaSet", nodes: 1});
    configRS.startSet({configsvr: '', storageEngine: 'wiredTiger'});
    configRS.initiate();

    jsTest.log("Adding a config server replica set without a specified shardName should fail.");
    addShardRes = st.s.adminCommand({addShard: configRS.getURL()});
    assertAddShardFailed(addShardRes);

    jsTest.log(
        "Adding a config server replica set with a shardName that matches the set's name should fail.");
    addShardRes = st.s.adminCommand({addShard: configRS.getURL(), name: configRS.name});
    assertAddShardFailed(addShardRes, configRS.name);

    jsTest.log(
        "Adding a config server replica set even with a non-'config' shardName should fail.");
    addShardRes = st.s.adminCommand({addShard: configRS.getURL(), name: "nonConfig"});
    assertAddShardFailed(addShardRes, "nonConfig");

    configRS.stopSet();

    st.stop();

})();
