/**
 * Test to assert that the RSM behaves correctly when contacting the primary node fails in various
 * ways.
 *
 * Restarts the config server in config shard suites, which requires persistence so restarted nodes
 * can rejoin their original replica set and run shutdown hooks.
 * @tags: [requires_persistence]
 */

// Checking UUID consistency and orphans involves talking to a shard node, which in this test is
// shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/rslib.js");

let st = new ShardingTest({shards: {rs0: {nodes: 1}}});
let mongos = st.s;
let rsPrimary = st.rs0.getPrimary();

// Make sure mongos knows who the primary is
awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: true, ismaster: true});

// Turn on the waitInHello failpoint. This will cause the primary node to cease sending isMaster
// responses and the RSM should mark the node as down
jsTestLog("Turning on waitInHello failpoint. Node should stop sending isMaster responses.");
const helloFailpoint = configureFailPoint(rsPrimary, "waitInHello");
awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: false, ismaster: false});
helloFailpoint.off();

// Wait for mongos to find out the node is still primary
awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: true, ismaster: true});

// Force the primary node to fail all isMaster requests. The RSM should mark the node as down.
jsTestLog("Turning on failCommand failpoint. Node should fail all isMaster responses.");
const failCmdFailpoint = configureFailPoint(
    rsPrimary,
    "failCommand",
    {errorCode: ErrorCodes.CommandFailed, failCommands: ["isMaster"], failInternalCommands: true});
awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: false, ismaster: false});
failCmdFailpoint.off();

// Wait for mongos to find out the node is still primary
awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: true, ismaster: true});

// Force the primary node to end the isMaster stream by not setting the 'moreToCome' bit on the
// resposne. The RSM should not mark the server as down or unknown and should continue monitoring
// the node.
jsTestLog(
    "Turning on doNotSetMoreToCome failpoint. Node should return successful isMaster responses.");
const moreToComeFailpoint = configureFailPoint(rsPrimary, "doNotSetMoreToCome");
// Wait for maxAwaitTimeMS to guarantee that mongos has received at least one isMaster response from
// the primary without the moreToCome bit set.
sleep(10000);
awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: true, ismaster: true});
moreToComeFailpoint.off();

// Wait for mongos to find out the node is still primary
awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: true, ismaster: true});

// Shutdown the primary node. The RSM should mark the node as down.
jsTestLog("Shutting down primary node.");
if (TestData.configShard) {
    st.rs0.stop(0, undefined, undefined, {forRestart: true});
} else {
    st.rs0.stop(0);
}
awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: false});

if (TestData.configShard) {
    // Shard0 is the config server in config shard mode, so restart it for the ShardingTest
    // shutdown hooks.
    st.rs0.start(0, undefined, true /* restart */);
}

st.stop();
}());
