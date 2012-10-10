/**
 * Stress test for ReplicaSetMonitor. Basically test removing a node
 * from the set with different nodes as the current primary and makes
 * sure ReplSetMonitor doesn't crash.
 */

var NODE_COUNT = 5;
var st = new ShardingTest({ shards: { rs0: { nodes: NODE_COUNT, oplogSize: 10 }},
    separateConfig: true });
var replTest = st.rs0;
var mongos = st.s;

// prevent the balancer from talking to the shards, which can trigger a replica set
// view refresh
st.stopBalancer();

mongos.getDB('test').user.find().explain();

// Iterate from the last node first as this is the index that is most prone to error
for (var node = NODE_COUNT - 1; node >= 0; node--) {
    jsTest.log('Iteration to remove node ' + node + '/' + (NODE_COUNT - 1));
    var connPoolStats = mongos.getDB('admin').runCommand({ connPoolStats: 1 });
    var targetHostName = connPoolStats['replicaSets'][replTest.name].hosts[node].addr;

    var priConn = replTest.getPrimary();
    var origConfDoc = priConn.getDB('local').system.replset.findOne();
    var confDoc = {};

    // Do a deep copy of the config doc
    Object.extend(confDoc, origConfDoc, true);

    // Force target to become primary
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

    assert.soon(function() {
        var connPoolStats = mongos.getDB('admin').runCommand({ connPoolStats: 1 });
        var replView = connPoolStats.replicaSets[replTest.name];
        print('Current replView: ' + tojson(replView));
        return replView.master == node;
    }, 'timed out waiting for node ' + node + ' to become master', 60000);

    // Remove first node from set
    confDoc.members.shift();
    confDoc.version++;

    jsTest.log('Removing node, new conf: ' + tojson(confDoc));

    assert.soon(function() {
        try {
            replTest.getPrimary().getDB('admin').adminCommand({ replSetReconfig: confDoc });
            var newConf = replTest.getPrimary().getDB('local').system.replset.findOne();
            print('Current config after attempting to remove: ' + tojson(newConf));
            return newConf.members.length == confDoc.members.length;
        } catch (x) {
            print('Expected exception because of reconfig: ' + x);
            return false;
        }
    }, 'timed out trying to remove node from config');

    var waitNodeCount = function(count) {
        var connPoolStats = mongos.getDB('admin').runCommand('connPoolStats');
        var replView = connPoolStats.replicaSets[replTest.name].hosts;
        print('current replView: ' + tojson(replView));

        return replView.length == count;
    };

    assert.soon(function() {
        /* TODO: SERVER-5175
         * 1. Trigger/Add sleep right before _master gets updated to the current value
         * 2. Trigger/Add sleep right before acquiring the lock inside getMaster. Sleep
         *    should be long enough to time with ReplicaSetMonitorWatcher refresh and
         *    not too long to let sleep at #1 expire.
         */
        try {
            mongos.getDB('test').user.find().explain();
        } catch (x) {
            print('query error, try again: ' + x);
        }

        return waitNodeCount(NODE_COUNT - 1);
    }, 'timed out waiting for node to be removed', 60000);

    jsTest.log(tojson(mongos.getDB('admin').runCommand('connPoolStats')));

    origConfDoc.version = confDoc.version + 1;
    jsTest.log('Put node back, new conf: ' + tojson(origConfDoc));

    try {
        replTest.getPrimary().getDB('admin').adminCommand({ replSetReconfig: origConfDoc });
    } catch (x) {
        print('Expected exception because of replSetReconfig: ' + x);
    }

    replTest.awaitSecondaryNodes();

    // Make sure mongos view of replica set is in steady state before proceeding
    assert.soon(function() { return waitNodeCount(NODE_COUNT); },
        'timed out waiting for node to get back to set', 60000);
}

// Make sure that mongos did not crash
assert(mongos.getDB('admin').runCommand({ serverStatus: 1 }).ok);

st.stop();

