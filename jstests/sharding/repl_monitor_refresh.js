/**
 * Test for making sure that the replica seed list in the config server does not
 * become invalid when a replica set reconfig happens.
 */

var NODE_COUNT = 3;
var st = new ShardingTest({ shards: { rs0: { nodes: NODE_COUNT, oplogSize: 10 }},
    separateConfig: true, config : 3 });
var replTest = st.rs0;
var mongos = st.s;

var shardDoc = mongos.getDB('config').shards.findOne();
assert.eq(NODE_COUNT, shardDoc.host.split(',').length); // seed list should contain all nodes

/* Make sure that the first node is not the primary (by making the second one primary).
 * We need to do this since the ReplicaSetMonitor iterates over the nodes one
 * by one and you can't remove a node that is currently the primary.
 */
var connPoolStats = mongos.getDB('admin').runCommand({ connPoolStats: 1 });
var targetHostName = connPoolStats['replicaSets'][replTest.name].hosts[1].addr;

var priConn = replTest.getPrimary();
var confDoc = priConn.getDB("local").system.replset.findOne();

for (var idx = 0; idx < confDoc.members.length; idx++) {
    if (confDoc.members[idx].host == targetHostName) {
        confDoc.members[idx].priority = 100;
    }
    else {
        confDoc.members[idx].priority = 1;
    }
}

confDoc.version++;

jsTest.log('Changing conf to ' + tojson(confDoc));

try {
    priConn.getDB('admin').adminCommand({ replSetReconfig: confDoc });
} catch (x) {
    print('Expected exception because of reconfig' + x);
}

ReplSetTest.awaitRSClientHosts(mongos, { host: targetHostName },
    { ok: true, ismaster: true });

// Remove first node from set
confDoc.members.shift();
confDoc.version++;

try {
    replTest.getPrimary().getDB('admin').adminCommand({ replSetReconfig: confDoc });
} catch (x) {
    print('Expected exception because of reconfig: ' + x);
}

assert.soon(function() {
    var connPoolStats = mongos.getDB('admin').runCommand('connPoolStats');
    var replView = connPoolStats.replicaSets[replTest.name].hosts;
    jsTest.log('current replView: ' + tojson(replView));

    return replView.length == NODE_COUNT - 1;
});

assert.soon(function() {
    shardDoc = mongos.getDB('config').shards.findOne();
    jsTest.log('shardDoc: ' + tojson(shardDoc));
    // seed list should contain one less node
    return shardDoc.host.split(',').length == NODE_COUNT - 1;
});

st.stop();

