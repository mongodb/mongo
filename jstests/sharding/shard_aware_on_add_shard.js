/**
 * Tests that the addShard process initializes sharding awareness on an added standalone or
 * replica set shard that was started with --shardsvr.
 */

(function() {
"use strict";

const checkShardingStateInitialized = function(conn, configConnStr, shardName, clusterId) {
    const res = conn.getDB('admin').runCommand({shardingState: 1});
    assert.commandWorked(res);
    assert(res.enabled);
    assert.eq(shardName, res.shardName);
    assert(clusterId.equals(res.clusterId),
           'cluster id: ' + tojson(clusterId) + ' != ' + tojson(res.clusterId));
    assert.soon(() => configConnStr == conn.adminCommand({shardingState: 1}).configServer);
};

const checkShardMarkedAsShardAware = function(mongosConn, shardName) {
    const res = mongosConn.getDB('config').getCollection('shards').findOne({_id: shardName});
    assert.neq(null, res, "Could not find new shard " + shardName + " in config.shards");
    assert.eq(1, res.state);
};

// Create the cluster to test adding shards to.
const st = new ShardingTest({shards: 1});
const clusterId = st.s.getDB('config').getCollection('version').findOne().clusterId;
const newShardName = "newShard";

// Add a shard and ensure awareness.
const replTest = new ReplSetTest({nodes: 1});
replTest.startSet({shardsvr: ''});
replTest.initiate();

jsTest.log("Going to add replica set as shard: " + tojson(replTest));
assert.commandWorked(st.s.adminCommand({addShard: replTest.getURL(), name: newShardName}));
checkShardingStateInitialized(replTest.getPrimary(), st.configRS.getURL(), newShardName, clusterId);
checkShardMarkedAsShardAware(st.s, newShardName);

replTest.stopSet();

st.stop();
})();
