/*
 * Tests that mongos does not mark nodes as down when reads or pings fail.
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");

/*
 * Configures failCommand to force the given command to fail with the given error code when run
 * on the given namespace, and returns the resulting fail point.
 */
function configureFailCommand(nodeConn, command, ns, error) {
    return configureFailPoint(
        nodeConn,
        "failCommand",
        {failInternalCommands: true, errorCode: error, failCommands: [command], namespace: ns});
}

const st = new ShardingTest({shards: 1, rs: {nodes: [{}, {rsConfig: {priority: 0}}]}});
const kDbName = "foo";
const kCollName = "bar";
const kNs = kDbName + "." + kCollName;
const kErrorCode = ErrorCodes.HostUnreachable;
const kPingWaitTimeMS = 12 * 1000;  // streamable ReplicaSetMonitor's ping interval is 10 seconds.
const testDB = st.s.getDB(kDbName);

assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: kNs, key: {x: 1}}));

jsTest.log("Test that mongos does not mark the primary node as down when reads fail");
(() => {
    const cmdObj = {
        count: kCollName,
        query: {x: {$gte: 0}},
        $readPreference: {mode: "primary"},
    };

    // Make the command fail on the primary node with HostUnreachable.
    const failPoint = configureFailCommand(st.rs0.nodes[0], "count", kNs, kErrorCode);
    assert.commandFailedWithCode(testDB.runCommand(cmdObj), kErrorCode);
    failPoint.off();

    // Verify that the node was not marked as down (i.e. it is still the primary node and the
    // command with readPreference "primary" works).
    awaitRSClientHosts(st.s, st.rs0.nodes[0], {ok: true, ismaster: true});
    assert.commandWorked(testDB.runCommand(cmdObj));
})();

jsTest.log("Test that mongos does not mark the secondary node as down when reads fail");
(() => {
    const cmdObj = {
        count: kCollName,
        query: {x: {$gte: 0}},
        $readPreference: {mode: "secondary"},
    };

    // Make the command fail on the secondary node.
    const failPoint = configureFailCommand(st.rs0.nodes[1], "count", kNs, kErrorCode);
    assert.commandFailedWithCode(testDB.runCommand(cmdObj), kErrorCode);
    failPoint.off();

    // Verify that the node was not marked as down (i.e. it is still the secondary node and the
    // command with readPreference "secondary" works).
    awaitRSClientHosts(st.s, st.rs0.nodes[1], {ok: true, secondary: true});
    assert.commandWorked(testDB.runCommand(cmdObj));
})();

jsTest.log("Test that mongos does not mark the primary node as down when pings fail");
(() => {
    // Make ping commands fail on the primary node with HostUnreachable. Sleep for some
    // time to allow the ServerPingMonitor to send out pings.
    const failPoint = configureFailPoint(
        st.s, "serverPingMonitorFailWithHostUnreachable", {hostAndPort: st.rs0.nodes[0].host});
    sleep(kPingWaitTimeMS);

    // Verify that the node was not marked as down (i.e. it is still the primary node and
    // we can run commands with readPreference "primary").
    awaitRSClientHosts(st.s, st.rs0.nodes[0], {ok: true, ismaster: true});
    const cmdObj = {
        count: kCollName,
        query: {x: {$gte: 0}},
        $readPreference: {mode: "primary"},
    };
    assert.commandWorked(testDB.runCommand(cmdObj));

    failPoint.off();
})();

jsTest.log("Test that mongos does not mark the secondary node as down when pings fail");
(() => {
    // Make ping commands fail on the secondary node with HostUnreachable. Sleep for some
    // time to allow the ServerPingMonitor to send out pings.
    const failPoint = configureFailPoint(
        st.s, "serverPingMonitorFailWithHostUnreachable", {hostAndPort: st.rs0.nodes[1].host});
    sleep(kPingWaitTimeMS);

    // Verify that the node was not marked as down (i.e. it is still the secondary node and
    // we can run commands with readPreference "secondary").
    awaitRSClientHosts(st.s, st.rs0.nodes[1], {ok: true, secondary: true});
    const cmdObj = {
        count: kCollName,
        query: {x: {$gte: 0}},
        $readPreference: {mode: "secondary"},
    };
    assert.commandWorked(testDB.runCommand(cmdObj));

    failPoint.off();
})();

st.stop();
})();
