load("jstests/libs/conn_pool_helpers.js");

/**
 * @tags: [requires_replication, requires_sharding, sets_replica_set_matching_strategy]
 */

(function() {
"use strict";

const kDbName = 'test';

const minConns = 4;
var stepParams = {
    ShardingTaskExecutorPoolMinSize: minConns,
    ShardingTaskExecutorPoolMaxSize: 10,
    ShardingTaskExecutorPoolMaxConnecting: 5,
    ShardingTaskExecutorPoolHostTimeoutMS: 300000,
    ShardingTaskExecutorPoolRefreshRequirementMS: 60000,
    ShardingTaskExecutorPoolRefreshTimeoutMS: 20000,
    ShardingTaskExecutorPoolReplicaSetMatching: "disabled",
};

const st = new ShardingTest({
    config: {nodes: 1},
    shards: 1,
    rs0: {nodes: 1},
    mongos: [{setParameter: stepParams}],
});
const mongos = st.s0;
const rst = st.rs0;
const primary = rst.getPrimary();

const cfg = primary.getDB('local').system.replset.findOne();
const allHosts = cfg.members.map(x => x.host);
const mongosDB = mongos.getDB(kDbName);
const primaryOnly = [primary.name];

var threads = [];
var currentCheckNum = 0;

function updateSetParameters(params) {
    var cmd = Object.assign({"setParameter": 1}, params);
    assert.commandWorked(mongos.adminCommand(cmd));
}

function dropConnections() {
    assert.commandWorked(mongos.adminCommand({dropConnections: 1, hostAndPort: allHosts}));
}

function resetPools() {
    dropConnections();
    mongos.adminCommand({multicast: {ping: 0}});
    currentCheckNum = assertHasConnPoolStats(mongos, allHosts, {ready: 4}, currentCheckNum);
}

function runSubTest(name, fun) {
    jsTestLog("Running test for " + name);

    resetPools();

    fun();

    updateSetParameters(stepParams);
}

assert.commandWorked(mongosDB.test.insert({x: 1}));
assert.commandWorked(mongosDB.test.insert({x: 2}));
assert.commandWorked(mongosDB.test.insert({x: 3}));
st.rs0.awaitReplication();

runSubTest("MinSize", function() {
    dropConnections();

    // Launch an initial find to trigger to min
    launchFinds(mongos, threads, {times: 1, readPref: "primary"});
    currentCheckNum = assertHasConnPoolStats(mongos, allHosts, {ready: minConns}, currentCheckNum);

    // Increase by one
    updateSetParameters({ShardingTaskExecutorPoolMinSize: 5});
    currentCheckNum = assertHasConnPoolStats(mongos, allHosts, {ready: 5}, currentCheckNum);

    // Increase to MaxSize
    updateSetParameters({ShardingTaskExecutorPoolMinSize: 10});
    currentCheckNum = assertHasConnPoolStats(mongos, allHosts, {ready: 10}, currentCheckNum);

    // Decrease to zero
    updateSetParameters({ShardingTaskExecutorPoolMinSize: 0});
});

runSubTest("MaxSize", function() {
    configureReplSetFailpoint(st, kDbName, "waitInFindBeforeMakingBatch", "alwaysOn");
    dropConnections();

    // Launch 10 blocked finds
    launchFinds(mongos, threads, {times: 10, readPref: "primary"});
    currentCheckNum =
        assertHasConnPoolStats(mongos, allHosts, {active: 10, hosts: primaryOnly}, currentCheckNum);

    // Increase by 5 and Launch another 4 blocked finds
    updateSetParameters({ShardingTaskExecutorPoolMaxSize: 15});
    launchFinds(mongos, threads, {times: 4, readPref: "primary"});
    currentCheckNum =
        assertHasConnPoolStats(mongos, allHosts, {active: 14, hosts: primaryOnly}, currentCheckNum);

    // Launch yet another 2, these should add only 1 connection
    launchFinds(mongos, threads, {times: 2, readPref: "primary"});
    currentCheckNum =
        assertHasConnPoolStats(mongos, allHosts, {active: 15, hosts: primaryOnly}, currentCheckNum);

    configureReplSetFailpoint(st, kDbName, "waitInFindBeforeMakingBatch", "off");
    currentCheckNum = assertHasConnPoolStats(
        mongos, allHosts, {ready: 15, pending: 0, hosts: primaryOnly}, currentCheckNum);
});

// Test maxConnecting
runSubTest("MaxConnecting", function() {
    const maxPending1 = 2;
    const maxPending2 = 4;
    const conns = 6;

    updateSetParameters({
        ShardingTaskExecutorPoolMaxSize: 100,
        ShardingTaskExecutorPoolMaxConnecting: maxPending1,
    });

    configureReplSetFailpoint(st, kDbName, "waitInHello", "alwaysOn");
    configureReplSetFailpoint(st, kDbName, "waitInFindBeforeMakingBatch", "alwaysOn");
    dropConnections();

    // Go to the limit of maxConnecting, so we're stuck here
    launchFinds(mongos, threads, {times: maxPending1, readPref: "primary"});
    currentCheckNum =
        assertHasConnPoolStats(mongos, allHosts, {pending: maxPending1}, currentCheckNum);

    // More won't run right now
    launchFinds(mongos, threads, {times: conns - maxPending1, readPref: "primary"});
    currentCheckNum =
        assertHasConnPoolStats(mongos, allHosts, {pending: maxPending1}, currentCheckNum);

    // If we increase our limit, it should fill in some of the connections
    updateSetParameters({ShardingTaskExecutorPoolMaxConnecting: maxPending2});
    currentCheckNum =
        assertHasConnPoolStats(mongos, allHosts, {pending: maxPending2}, currentCheckNum);

    // Dropping the limit doesn't cause us to drop pending
    updateSetParameters({ShardingTaskExecutorPoolMaxConnecting: maxPending1});
    currentCheckNum =
        assertHasConnPoolStats(mongos, allHosts, {pending: maxPending2}, currentCheckNum);

    // Release our pending and walk away
    configureReplSetFailpoint(st, kDbName, "waitInHello", "off");
    currentCheckNum =
        assertHasConnPoolStats(mongos,
                               allHosts,
                               {
                                   // Expects the number of pending connections to be zero.
                                   checkStatsFunc: function(stats) {
                                       return stats.refreshing == 0;
                                   }
                               },
                               currentCheckNum);
    configureReplSetFailpoint(st, kDbName, "waitInFindBeforeMakingBatch", "off");
});

runSubTest("Timeouts", function() {
    const conns = minConns;
    const pendingTimeoutMS = 5000;
    const toRefreshTimeoutMS = 1000;
    const idleTimeoutMS1 = 20000;
    const idleTimeoutMS2 = 15500;

    // Updating separately since the validation depends on existing params
    updateSetParameters({
        ShardingTaskExecutorPoolRefreshTimeoutMS: pendingTimeoutMS,
    });
    updateSetParameters({
        ShardingTaskExecutorPoolRefreshRequirementMS: toRefreshTimeoutMS,
    });
    updateSetParameters({
        ShardingTaskExecutorPoolHostTimeoutMS: idleTimeoutMS1,
    });

    configureReplSetFailpoint(st, kDbName, "waitInFindBeforeMakingBatch", "alwaysOn");
    dropConnections();

    // Make ready connections
    launchFinds(mongos, threads, {times: conns, readPref: "primary"});
    configureReplSetFailpoint(st, kDbName, "waitInFindBeforeMakingBatch", "off");
    currentCheckNum = assertHasConnPoolStats(mongos, allHosts, {ready: conns}, currentCheckNum);

    // Block refreshes and wait for the toRefresh timeout
    configureReplSetFailpoint(st, kDbName, "waitInHello", "alwaysOn");
    sleep(toRefreshTimeoutMS);

    // Confirm that we're in pending for all of our conns
    currentCheckNum = assertHasConnPoolStats(mongos, allHosts, {pending: conns}, currentCheckNum);

    // Set our min conns to 0 to make sure we don't refresh after pending timeout
    updateSetParameters({
        ShardingTaskExecutorPoolMinSize: 0,
    });

    // Wait for our pending timeout
    sleep(pendingTimeoutMS);
    currentCheckNum = assertHasConnPoolStats(mongos, allHosts, {}, currentCheckNum);

    configureReplSetFailpoint(st, kDbName, "waitInHello", "off");

    // Reset the min conns to make sure normal refresh doesn't extend the timeout
    updateSetParameters({
        ShardingTaskExecutorPoolMinSize: minConns,
    });

    // Wait for our host timeout and confirm the pool drops
    sleep(idleTimeoutMS1);
    currentCheckNum = assertHasConnPoolStats(mongos, allHosts, {isAbsent: true}, currentCheckNum);

    // Reset the pool
    resetPools();

    // Sleep for a shorter timeout and then update so we're already expired
    sleep(idleTimeoutMS2);
    updateSetParameters({ShardingTaskExecutorPoolHostTimeoutMS: idleTimeoutMS2});
    currentCheckNum = assertHasConnPoolStats(mongos, allHosts, {isAbsent: true}, currentCheckNum);
});

threads.forEach(function(thread) {
    thread.join();
});

st.stop();
})();
