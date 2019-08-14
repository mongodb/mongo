load("jstests/libs/parallelTester.js");

/**
 * @tags: [requires_sharding]
 */

(function() {
"use strict";

const st = new ShardingTest({mongos: 1, shards: 1, rs: {nodes: 2, protocolVersion: 1}});
const kDbName = 'test';
const mongosClient = st.s;
const mongos = mongosClient.getDB(kDbName);
const rst = st.rs0;
const primary = rst.getPrimary();
const secondary = rst.getSecondaries()[0];

const cfg = primary.getDB('local').system.replset.findOne();
const allHosts = cfg.members.map(x => x.host);
const primaryOnly = [primary.name];
const secondaryOnly = [secondary.name];

function configureReplSetFailpoint(name, modeValue) {
    st.rs0.nodes.forEach(function(node) {
        assert.commandWorked(node.getDB("admin").runCommand({
            configureFailPoint: name,
            mode: modeValue,
            data: {shouldCheckForInterrupt: true},
        }));
    });
}

var threads = [];

function launchFinds({times, readPref, shouldFail}) {
    jsTestLog("Starting " + times + " connections");
    for (var i = 0; i < times; i++) {
        var thread = new Thread(function(connStr, readPref, dbName, shouldFail) {
            var client = new Mongo(connStr);
            const ret = client.getDB(dbName).runCommand(
                {find: "test", limit: 1, "$readPreference": {mode: readPref}});

            if (shouldFail) {
                assert.commandFailed(ret);
            } else {
                assert.commandWorked(ret);
            }
        }, st.s.host, readPref, kDbName, shouldFail);
        thread.start();
        threads.push(thread);
    }
}

function updateSetParameters(params) {
    var cmd = Object.assign({"setParameter": 1}, params);
    assert.commandWorked(mongos.adminCommand(cmd));
}

function dropConnections() {
    assert.commandWorked(mongos.adminCommand({dropConnections: 1, hostAndPort: allHosts}));
}

var currentCheckNum = 0;
function hasConnPoolStats(args) {
    const checkNum = currentCheckNum++;
    jsTestLog("Check #" + checkNum + ": " + tojson(args));
    var {ready, pending, active, hosts, isAbsent} = args;

    ready = ready ? ready : 0;
    pending = pending ? pending : 0;
    active = active ? active : 0;
    hosts = hosts ? hosts : allHosts;

    function checkStats(res, host) {
        var stats = res.hosts[host];
        if (!stats) {
            jsTestLog("Connection stats for " + host + " are absent");
            return isAbsent;
        }

        jsTestLog("Connection stats for " + host + ": " + tojson(stats));
        return stats.available == ready && stats.refreshing == pending && stats.inUse == active;
    }

    function checkAllStats() {
        var res = mongos.adminCommand({connPoolStats: 1});
        return hosts.map(host => checkStats(res, host)).every(x => x);
    }

    assert.soon(checkAllStats, "Check #" + checkNum + " failed", 10000);

    jsTestLog("Check #" + checkNum + " successful");
}

function checkConnPoolStats() {
    const ret = mongos.runCommand({"connPoolStats": 1});
    const poolStats = ret["pools"]["NetworkInterfaceTL-TaskExecutorPool-0"];
    jsTestLog(poolStats);
}

function walkThroughBehavior({primaryFollows, secondaryFollows}) {
    // Start pooling with a ping
    mongos.adminCommand({multicast: {ping: 0}});
    checkConnPoolStats();

    // Block connections from finishing
    configureReplSetFailpoint("waitInFindBeforeMakingBatch", "alwaysOn");

    // Launch a bunch of primary finds
    launchFinds({times: 10, readPref: "primary"});

    // Confirm we follow
    hasConnPoolStats({active: 10, hosts: primaryOnly});
    if (secondaryFollows) {
        hasConnPoolStats({ready: 10, hosts: secondaryOnly});
    }
    checkConnPoolStats();

    // Launch a bunch of secondary finds
    launchFinds({times: 20, readPref: "secondary"});

    // Confirm we follow
    hasConnPoolStats({active: 20, hosts: secondaryOnly});
    if (primaryFollows) {
        hasConnPoolStats({ready: 10, active: 10, hosts: primaryOnly});
    }
    checkConnPoolStats();

    configureReplSetFailpoint("waitInFindBeforeMakingBatch", "off");

    dropConnections();
}

assert.commandWorked(mongos.test.insert({x: 1}));
assert.commandWorked(mongos.test.insert({x: 2}));
assert.commandWorked(mongos.test.insert({x: 3}));
st.rs0.awaitReplication();

jsTestLog("Following disabled");
walkThroughBehavior({primaryFollows: false, secondaryFollows: false});

jsTestLog("Following primary node");
updateSetParameters({ShardingTaskExecutorPoolReplicaSetMatching: "matchPrimaryNode"});
walkThroughBehavior({primaryFollows: false, secondaryFollows: true});

// jsTestLog("Following busiest node");
// updateSetParameters({ShardingTaskExecutorPoolReplicaSetMatching: "matchBusiestNode"});
// walkThroughBehavior({primaryFollows: true, secondaryFollows: true});

jsTestLog("Reseting to disabled");
updateSetParameters({ShardingTaskExecutorPoolReplicaSetMatching: "disabled"});
walkThroughBehavior({primaryFollows: false, secondaryFollows: false});

threads.forEach(function(thread) {
    thread.join();
});

st.stop();
})();
