// Tests that mongos and shard mongods can both be started up successfully when there is no config
// server, and that they will wait until there is a config server online before handling any
// sharding operations.
//
// This test involves restarting a standalone shard, so cannot be run on ephemeral storage engines.
// A restarted standalone will lose all data when using an ephemeral storage engine.
// @tags: [requires_persistence]
(function() {
    "use strict";

    /**
     * Restarts the mongod backing the specified shard instance, without restarting the mongobridge.
     */
    function restartShard(shard, waitForConnect) {
        MongoRunner.stopMongod(shard);
        shard.restart = true;
        shard.waitForConnect = waitForConnect;
        MongoRunner.runMongod(shard);
    }

    var st = new ShardingTest({shards: 2});

    jsTestLog("Setting up initial data");

    for (var i = 0; i < 100; i++) {
        assert.writeOK(st.s.getDB('test').foo.insert({_id: i}));
    }

    assert.commandWorked(st.s0.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', 'shard0000');

    assert.commandWorked(st.s0.adminCommand({shardCollection: 'test.foo', key: {_id: 1}}));
    assert.commandWorked(st.s0.adminCommand({split: 'test.foo', find: {_id: 50}}));
    assert.commandWorked(
        st.s0.adminCommand({moveChunk: 'test.foo', find: {_id: 75}, to: 'shard0001'}));

    // Make sure the pre-existing mongos already has the routing information loaded into memory
    assert.eq(100, st.s.getDB('test').foo.find().itcount());

    jsTestLog("Shutting down all config servers");
    for (var i = 0; i < st._configServers.length; i++) {
        st.stopConfigServer(i);
    }

    jsTestLog("Starting a new mongos when there are no config servers up");
    var newMongosInfo = MongoRunner.runMongos({configdb: st._configDB, waitForConnect: false});
    // The new mongos won't accept any new connections, but it should stay up and continue trying
    // to contact the config servers to finish startup.
    assert.throws(function() {
        new Mongo(newMongosInfo.host);
    });

    jsTestLog("Restarting a shard while there are no config servers up");
    restartShard(st.shard1, false);

    jsTestLog("Queries should fail because the shard can't initialize sharding state");
    var error = assert.throws(function() {
        st.s.getDB('test').foo.find().itcount();
    });

    assert(ErrorCodes.ReplicaSetNotFound == error.code ||
           ErrorCodes.ExceededTimeLimit == error.code || ErrorCodes.HostUnreachable == error.code);

    jsTestLog("Restarting the config servers");
    for (var i = 0; i < st._configServers.length; i++) {
        st.restartConfigServer(i);
    }

    print("Sleeping for 60 seconds to let the other shards restart their ReplicaSetMonitors");
    sleep(60000);

    jsTestLog("Queries against the original mongos should work again");
    assert.eq(100, st.s.getDB('test').foo.find().itcount());

    jsTestLog("Should now be possible to connect to the mongos that was started while the config " +
              "servers were down");
    var newMongosConn = null;
    var caughtException = null;
    assert.soon(
        function() {
            try {
                newMongosConn = new Mongo(newMongosInfo.host);
                return true;
            } catch (e) {
                caughtException = e;
                return false;
            }
        },
        "Failed to connect to mongos after config servers were restarted: " +
            tojson(caughtException));

    assert.eq(100, newMongosConn.getDB('test').foo.find().itcount());

    st.stop();
}());
