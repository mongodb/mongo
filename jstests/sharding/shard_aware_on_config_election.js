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
        // TODO: SERVER-22665 a mixed-version test should be written specifically testing receiving
        // addShard from a legacy mongos, and this assert.soon() should be changed back to assert
        // synchronously.
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

    var checkShardMarkedAsShardAware = function(mongosConn, shardName) {
        // TODO: SERVER-22665 a mixed-version test should be written specifically testing receiving
        // addShard from a legacy mongos, and this assert.soon() should be changed back to assert
        // synchronously.
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

        jsTest.log("Manually delete the shardIdentity document from " + rst.name);
        // Deleting the shardIdentity document allows for clearing sharding awareness on restarting
        // the shard (without the shardIdentity document, the shard will not perform sharding
        // initialization).
        // Use writeConcern: { w: majority } so that the write cannot be lost when the shard is
        // restarted.
        assert.writeOK(rst.getPrimary()
                           .getDB("admin")
                           .getCollection("system.version")
                           .remove({"_id": "shardIdentity"}, {writeConcern: {w: "majority"}}));

        jsTest.log("Manually unset the state field from " + rst.name + "'s entry in config.shards");
        // Use writeConcern: { w: majority } so that the write cannot be rolled back when the
        // current primary is stepped down.
        assert.writeOK(st.c0.getDB("config").getCollection("shards").update(
            {"_id": rst.name}, {$unset: {"state": ""}}, {writeConcern: {w: "majority"}}));

        jsTest.log("Restart " + rst.name + " to clear its sharding awareness");
        // Restart with fullOptions to ensure node is restarted with --shardsvr.
        for (var nodeId = 0; nodeId < rst.nodes.length; nodeId++) {
            rst.restart(nodeId, rst.nodes[nodeId].fullOptions);
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
        checkShardingStateInitialized(rst.getPrimary(), st.configRS.getURL(), rst.name, clusterId);
        checkShardMarkedAsShardAware(st.s, rst.name);
    }

    st.stop();

})();
