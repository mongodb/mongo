/**
 * Test to check that the RSM receives a hello reply "immediately" (or "quickly") after a RS
 * topology change when using the exhaust protocol. In order to test this, we'll set the
 * maxAwaitTimeMS to much higher than the default (5 mins). This will allow us to assert that the
 * RSM receives the hello replies because of a topology change rather than maxAwaitTimeMS being
 * hit. A replica set node should send a response to the mongos as soon as it processes a topology
 * change, so "immediately"/"quickly" can vary - we specify 5 seconds in this test ('timeoutMS').
 *
 * @tags: [requires_streamable_rsm]
 */

// This test shuts down a shard's node and because of this consistency checking
// cannot be performed on that node, which causes the consistency checker to fail.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

import {awaitRSClientHosts} from "jstests/replsets/rslib.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let overrideMaxAwaitTimeMS = {'mode': 'alwaysOn', 'data': {maxAwaitTimeMS: 5 * 60 * 1000}};
let st = new ShardingTest({
    mongos:
        {s0: {setParameter: {"failpoint.overrideMaxAwaitTimeMS": tojson(overrideMaxAwaitTimeMS)}}},
    shards: {rs0: {nodes: [{}, {}, {rsConfig: {priority: 0}}]}},
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true
});

let timeoutMS = 20000;
let mongos = st.s;
let rsPrimary = st.rs0.getPrimary();
let electableRsSecondary;
st.rs0.getReplSetConfig().members.forEach(node => {
    if (node.priority != 0 && node.host != rsPrimary.host) {
        electableRsSecondary = st.rs0.nodes[node._id];
    }
});

// Make sure mongos knows who the primary is
awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: true, ismaster: true});

// Force the secondary to step up and trigger a topology change
jsTestLog("Stepping up a new primary node.");
st.rs0.stepUp(electableRsSecondary);
assert.eq(electableRsSecondary, st.rs0.getPrimary());
st.rs0.awaitSecondaryNodes(null, [rsPrimary]);
awaitRSClientHosts(
    mongos, {host: electableRsSecondary.name}, {ok: true, ismaster: true}, st.rs0, timeoutMS);
awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: true, ismaster: false}, st.rs0, timeoutMS);

// Get connections to the new primary and electable secondary. Also, update the primary's member
// tags. This should trigger a topology change
jsTestLog("Updating the primary node's member tags.");
rsPrimary = st.rs0.getPrimary();
let config = rsPrimary.getDB("local").system.replset.findOne();
config.members.forEach(node => {
    if (node.priority != 0 && node.host != rsPrimary.host) {
        electableRsSecondary = st.rs0.nodes[node._id];
    }
    if (node.host == rsPrimary.name) {
        node.tags = {dc: "ny"};
    }
});
config.version = (config.version || 0) + 1;
assert.commandWorked(rsPrimary.getDB('admin').runCommand({replSetReconfig: config}));
awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: true, tags: {dc: "ny"}}, st.rs0, timeoutMS);

// Kill the primary and wait for the secondary to step up, trigger a topology change
jsTestLog("Killing the primary.");
st.rs0.stop(rsPrimary);
st.rs0.waitForState(electableRsSecondary, ReplSetTest.State.PRIMARY);
awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: false}, st.rs0, timeoutMS);
awaitRSClientHosts(
    mongos, {host: electableRsSecondary.name}, {ok: true, ismaster: true}, st.rs0, timeoutMS);

// Restart the node we shut down
jsTestLog("Restarting the node that was just killed.");
st.rs0.start(rsPrimary, {}, true /* restart */);

rsPrimary = st.rs0.getPrimary();
st.rs0.getReplSetConfig().members.forEach(node => {
    if (node.priority != 0 && node.host != rsPrimary.host) {
        electableRsSecondary = st.rs0.nodes[node._id];
    }
});

jsTestLog("Wait for the electable secondary to reach the SECONDARY after initial sync.");
st.rs0.awaitSecondaryNodes(null, [electableRsSecondary]);

// Terminate the primary and wait for the secondary to step up, trigger a topology change
jsTestLog("Terminating the primary.");
st.rs0.stop(rsPrimary, 15);
st.rs0.waitForState(electableRsSecondary, ReplSetTest.State.PRIMARY);
awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: false}, st.rs0, timeoutMS);
awaitRSClientHosts(
    mongos, {host: electableRsSecondary.name}, {ok: true, ismaster: true}, st.rs0, timeoutMS);

st.stop();
