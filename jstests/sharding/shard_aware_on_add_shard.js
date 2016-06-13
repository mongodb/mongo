/**
 * Tests that the addShard process initializes sharding awareness on an added standalone or
 * replica set shard that was started with --shardsvr.
 */

(function() {
    "use strict";

    var waitForIsMaster = function(conn) {
        assert.soon(function() {
            var res = conn.getDB('admin').runCommand({isMaster: 1});
            return res.ismaster;
        });
    };

    var checkShardingStateInitialized = function(conn, configConnStr, shardName) {
        var res = conn.getDB('admin').runCommand({shardingState: 1});
        assert.commandWorked(res);
        assert(res.enabled);
        assert.eq(configConnStr, res.configServer);
        assert.eq(shardName, res.shardName);
        // TODO SERVER-23096: How should the clusterId be obtained externally?
        // assert.eq(clusterId, res.clusterId);
    };

    // Create the cluster to test adding shards to.
    var st = new ShardingTest({shards: 1});

    // Add a shard that is a standalone mongod.

    var standaloneConn = MongoRunner.runMongod({shardsvr: ''});
    waitForIsMaster(standaloneConn);

    jsTest.log("Going to add standalone as shard: " + standaloneConn);
    var newShardName = "newShard";
    assert.commandWorked(st.s.adminCommand({addShard: standaloneConn.name, name: newShardName}));
    checkShardingStateInitialized(standaloneConn, st.configRS.getURL(), newShardName);

    MongoRunner.stopMongod(standaloneConn.port);

    // Add a shard that is a replica set.

    var replTest = new ReplSetTest({nodes: 1});
    replTest.startSet({shardsvr: ''});
    replTest.initiate();
    waitForIsMaster(replTest.getPrimary());

    jsTest.log("Going to add replica set as shard: " + replTest);
    assert.commandWorked(st.s.adminCommand({addShard: replTest.getURL(), name: replTest.getURL()}));
    checkShardingStateInitialized(replTest.getPrimary(), st.configRS.getURL(), replTest.getURL());

    replTest.stopSet();

    st.stop();

})();
