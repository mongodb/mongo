load("jstests/replsets/rslib.js");

/**
 * Test for making sure that the replica seed list in the config server does not
 * become invalid when a replica set reconfig happens.
 * @tags: [multiversion_incompatible]
 */
(function() {
"use strict";

// Skip db hash check and shard replication since the removed node has wrong config and is still
// alive.
TestData.skipCheckDBHashes = true;
TestData.skipAwaitingReplicationOnShardsBeforeCheckingUUIDs = true;

var NODE_COUNT = 3;
var st = new ShardingTest({shards: {rs0: {nodes: NODE_COUNT, oplogSize: 10}}});
var replTest = st.rs0;
var mongos = st.s;

var shardDoc = mongos.getDB('config').shards.findOne();
assert.eq(NODE_COUNT, shardDoc.host.split(',').length);  // seed list should contain all nodes

/* Make sure that the first node is not the primary (by making the second one primary).
 * We need to do this since the ReplicaSetMonitor iterates over the nodes one
 * by one and you can't remove a node that is currently the primary.
 */
var connPoolStats = mongos.getDB('admin').runCommand({connPoolStats: 1});
var targetHostName = connPoolStats['replicaSets'][replTest.name].hosts[1].addr;

var priConn = replTest.getPrimary();
var confDoc = priConn.getDB("local").system.replset.findOne();

for (var idx = 0; idx < confDoc.members.length; idx++) {
    if (confDoc.members[idx].host == targetHostName) {
        confDoc.members[idx].priority = 100;
    } else {
        confDoc.members[idx].priority = 1;
    }
}

confDoc.version++;

jsTest.log('Changing conf to ' + tojson(confDoc));

reconfig(replTest, confDoc);

awaitRSClientHosts(mongos, {host: targetHostName}, {ok: true, ismaster: true});
let rsConfig = st.rs0.getReplSetConfigFromNode();
assert.soon(function() {
    const res = st.rs0.getPrimary().adminCommand({replSetGetStatus: 1});
    return ((res.members[0].configVersion === rsConfig.version) &&
            (res.members[2].configVersion === rsConfig.version) &&
            (res.members[0].configTerm === rsConfig.term) &&
            (res.members[2].configTerm === rsConfig.term));
});

// Remove first node from set
confDoc.members.shift();
confDoc.version++;

reconfig(replTest, confDoc);

jsTest.log("Waiting for mongos to reflect change in shard replica set membership.");
var replView;
assert.soon(
    function() {
        var connPoolStats = mongos.getDB('admin').runCommand('connPoolStats');
        replView = connPoolStats.replicaSets[replTest.name].hosts;
        return replView.length == confDoc.members.length;
    },
    function() {
        return ("Expected to find " + confDoc.members.length + " nodes but found " +
                replView.length + " in " + tojson(replView));
    });

jsTest.log("Waiting for config.shards to reflect change in shard replica set membership.");
assert.soon(
    function() {
        shardDoc = mongos.getDB('config').shards.findOne();
        // seed list should contain one less node
        return shardDoc.host.split(',').length == confDoc.members.length;
    },
    function() {
        return ("Expected to find " + confDoc.members.length + " nodes but found " +
                shardDoc.host.split(',').length + " in " + shardDoc.host);
    });

st.stop();
}());
