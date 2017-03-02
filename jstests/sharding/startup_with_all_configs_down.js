// Tests that bongos and shard bongods can both be started up successfully when there is no config
// server, and that they will wait until there is a config server online before handling any
// sharding operations.
//
// This test involves restarting a standalone shard, so cannot be run on ephemeral storage engines.
// A restarted standalone will lose all data when using an ephemeral storage engine.
// @tags: [requires_persistence]
(function() {
    "use strict";

    /**
     * Restarts the bongod backing the specified shard instance, without restarting the bongobridge.
     */
    function restartShard(shard, waitForConnect) {
        BongoRunner.stopBongod(shard);
        shard.restart = true;
        shard.waitForConnect = waitForConnect;
        BongoRunner.runBongod(shard);
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

    // Make sure the pre-existing bongos already has the routing information loaded into memory
    assert.eq(100, st.s.getDB('test').foo.find().itcount());

    jsTestLog("Shutting down all config servers");
    for (var i = 0; i < st._configServers.length; i++) {
        st.stopConfigServer(i);
    }

    jsTestLog("Starting a new bongos when there are no config servers up");
    var newBongosInfo = BongoRunner.runBongos({configdb: st._configDB, waitForConnect: false});
    // The new bongos won't accept any new connections, but it should stay up and continue trying
    // to contact the config servers to finish startup.
    assert.throws(function() {
        new Bongo(newBongosInfo.host);
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

    jsTestLog("Queries against the original bongos should work again");
    assert.eq(100, st.s.getDB('test').foo.find().itcount());

    jsTestLog("Should now be possible to connect to the bongos that was started while the config " +
              "servers were down");
    var newBongosConn = null;
    var caughtException = null;
    assert.soon(
        function() {
            try {
                newBongosConn = new Bongo(newBongosInfo.host);
                return true;
            } catch (e) {
                caughtException = e;
                return false;
            }
        },
        "Failed to connect to bongos after config servers were restarted: " +
            tojson(caughtException));

    assert.eq(100, newBongosConn.getDB('test').foo.find().itcount());

    st.stop();
}());
