/**
 * Tests that, on transition to primary, a config server initializes sharding awareness on all
 * shards not marked as sharding aware in config.shards.
 *
 * This test restarts shard and config server nodes.
 * @tags: [requires_persistence]
 */

(function() {
    "use strict";

    var waitForIsMaster = function(conn) {
        assert.soon(function() {
            var res = conn.getDB('admin').runCommand({isMaster: 1});
            return res.ismaster;
        });
    };

    var checkShardingStateInitialized = function(conn, configConnStr, shardName, clusterId) {
        var res = conn.getDB('admin').runCommand({shardingState: 1});
        assert.commandWorked(res);
        assert(res.enabled);
        assert.eq(configConnStr, res.configServer);
        assert.eq(shardName, res.shardName);
        assert(clusterId.equals(res.clusterId),
               'cluster id: ' + tojson(clusterId) + ' != ' + tojson(res.clusterId));
    };

    var checkShardMarkedAsShardAware = function(mongosConn, shardName) {
        var res = mongosConn.getDB('config').getCollection('shards').findOne({_id: shardName});
        assert.neq(null, res, "Could not find new shard " + shardName + " in config.shards");
        assert.eq(1, res.state);
    };

    var waitUntilShardingStateInitialized = function(conn, configConnStr, shardName, clusterId) {
        assert.soon(function() {
            var res = conn.getDB('admin').runCommand({shardingState: 1});
            assert.commandWorked(res);
            if (res.enabled && (configConnStr === res.configServer) &&
                (shardName === res.shardName) && (clusterId.equals(res.clusterId))) {
                return true;
            }
            return false;
        });
    };

    var waitUntilShardMarkedAsShardAware = function(mongosConn, shardName) {
        assert.soon(function() {
            var res = mongosConn.getDB('config').getCollection('shards').findOne({_id: shardName});
            assert.neq(null, res, "Could not find new shard " + shardName + " in config.shards");
            if (res.state && res.state === 1) {
                return true;
            }
            return false;
        });
    };

    var numShards = 2;
    var st = new ShardingTest({shards: numShards, other: {rs: true}});
    var clusterId = st.s.getDB('config').getCollection('version').findOne().clusterId;

    var restartedShards = [];
    for (var i = 0; i < numShards; i++) {
        var rst = st["rs" + i];

        jsTest.log("Assert that shard " + rst.name +
                   " is sharding aware and was marked as sharding aware in config.shards");
        checkShardingStateInitialized(rst.getPrimary(), st.configRS.getURL(), rst.name, clusterId);
        checkShardMarkedAsShardAware(st.s, rst.name);

        jsTest.log("Restart " + rst.name + " without --shardsvr to clear its sharding awareness");
        for (var nodeId = 0; nodeId < rst.nodes.length; nodeId++) {
            var rstOpts = rst.nodes[nodeId].fullOptions;
            delete rstOpts.shardsvr;
            rst.restart(nodeId, rstOpts);
        }
        rst.awaitNodesAgreeOnPrimary();

        jsTest.log("Manually delete the shardIdentity document from " + rst.name);
        // Use writeConcern: { w: majority } so that the write cannot be lost when the shard is
        // restarted again with --shardsvr.
        assert.writeOK(rst.getPrimary()
                           .getDB("admin")
                           .getCollection("system.version")
                           .remove({"_id": "shardIdentity"}, {writeConcern: {w: "majority"}}));

        jsTest.log("Manually unset the state field from " + rst.name + "'s entry in config.shards");
        // Use writeConcern: { w: majority } so that the write cannot be rolled back when the
        // current primary is stepped down.
        assert.writeOK(st.s.getDB("config").getCollection("shards").update(
            {"_id": rst.name}, {$unset: {"state": ""}}, {writeConcern: {w: "majority"}}));

        // Make sure shardIdentity delete replicated to all nodes before restarting them with
        // --shardsvr since if they try to replicate that delete while runnning with --shardsvr
        // they will crash.
        rst.awaitReplication();
        jsTest.log("Restart " + rst.name +
                   " with --shardsvr to allow initializing its sharding awareness");
        for (var nodeId = 0; nodeId < rst.nodes.length; nodeId++) {
            var rstOpts = rst.nodes[nodeId].fullOptions;
            rstOpts.shardsvr = "";
            rst.restart(nodeId, rstOpts);
        }
        rst.awaitNodesAgreeOnPrimary();
    }

    jsTest.log("Step down the primary config server");
    // Step down the primary config server so that the newly elected primary performs sharding
    // initialization on shards not marked as shard aware.
    assert.throws(function() {
        st.configRS.getPrimary().getDB("admin").runCommand({replSetStepDown: 10});
    });

    jsTest.log("Wait for a new primary config server to be elected.");
    st.configRS.awaitNodesAgreeOnPrimary();

    for (var i = 0; i < numShards; i++) {
        var rst = st["rs" + i];
        jsTest.log("Assert that shard " + rst.name +
                   " became sharding aware and marked as sharding aware in config.shards again");
        waitUntilShardingStateInitialized(
            rst.getPrimary(), st.configRS.getURL(), rst.name, clusterId);
        waitUntilShardMarkedAsShardAware(st.s, rst.name);
    }

    st.stop();

})();
