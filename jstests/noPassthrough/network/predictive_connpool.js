/**
 * @tags: [
 *   requires_sharding,
 *   sets_replica_set_matching_strategy,
 *   # Gets its host list based on a call to system.replset.findOne().
 *   grpc_incompatible,
 * ]
 */

import {configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

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

function assertSoonMongosHasConnPoolStats({hosts, ready = 0, pending = 0, active = 0}) {
    function checkStatsForHost({connPoolStatsResponse, host, mismatches}) {
        const stats = connPoolStatsResponse.hosts[host];
        if (!stats) {
            mismatches[host] = "Connection stats are absent";
            return;
        }

        const myMismatches = {};
        if (stats.available != ready) {
            myMismatches.available = {actual: stats.available, expected: ready};
        }
        if (stats.refreshing != pending) {
            myMismatches.refreshing = {actual: stats.refreshing, expected: pending};
        }
        if (stats.inUse != active) {
            myMismatches.inUse = {actual: stats.inUse, expected: active};
        }

        if (Object.keys(myMismatches).length !== 0) {
            mismatches[host] = myMismatches;
        }
    }

    let mismatches;
    let connPoolStatsResponse;

    function checkStatsAndThreads() {
        // If any of the "find" threads failed, then let that failure bubble up as an exception.
        threads.forEach(thread => {
            const status = thread.currentStatus();
            assert.eq(status.code, 0, "error occurred in 'find' thread: " + tojson(status));
        });

        // Return whether the connection pool stats for each of the `hosts` is as expected.
        // mismatches :: {[host]: {[field]: {actual, expected}, ...}, ...}
        // mismatches :: {[host]: "Connection stats are absent", ...}
        mismatches = {};
        connPoolStatsResponse = mongos.adminCommand({connPoolStats: 1});
        hosts.forEach(host => checkStatsForHost({connPoolStatsResponse, host, mismatches}));
        return Object.keys(mismatches).length === 0;
    }

    function makeErrorMessage() {
        const briefResponse =
            Object.fromEntries(Object.entries(connPoolStatsResponse.hosts).map(([host, stats]) => {
                delete stats.acquisitionWaitTimes;
                return [host, stats];
            }));
        return `connPoolStats response had mismatches:
${tojson(mismatches)}

Here is the most recent connPoolStats.hosts (with acquisitionWaitTimes omitted for brevity):
${tojson(briefResponse)}`;
    }

    assert.soon(checkStatsAndThreads, makeErrorMessage);
}

function walkThroughBehavior({primaryFollows, secondaryFollows}) {
    // Start pooling with a ping
    mongos.adminCommand({multicast: {ping: 0}});

    // Block connections from finishing
    const fpRs = configureFailPointForRS(st.rs0.nodes,
                                         "waitInFindBeforeMakingBatch",
                                         {shouldCheckForInterrupt: true, nss: kDbName + ".test"});

    // Launch a bunch of primary finds
    launchFinds({times: 10, readPref: "primary"});

    // Confirm we follow
    assertSoonMongosHasConnPoolStats({active: 10, hosts: primaryOnly});
    if (secondaryFollows) {
        assertSoonMongosHasConnPoolStats({ready: 10, hosts: secondaryOnly});
    }

    // Launch a bunch of secondary finds
    launchFinds({times: 20, readPref: "secondary"});

    // Confirm we follow
    assertSoonMongosHasConnPoolStats({active: 20, hosts: secondaryOnly});
    if (primaryFollows) {
        assertSoonMongosHasConnPoolStats({ready: 10, active: 10, hosts: primaryOnly});
    }

    fpRs.off();

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

jsTestLog("Reseting to disabled");
updateSetParameters({ShardingTaskExecutorPoolReplicaSetMatching: "disabled"});
walkThroughBehavior({primaryFollows: false, secondaryFollows: false});

threads.forEach(function(thread) {
    thread.join();
});

st.stop();
