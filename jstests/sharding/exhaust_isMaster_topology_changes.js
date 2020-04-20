/**
 * Test to check that the RSM receives an isMaster reply "immediately" (or "quickly") after a RS
 * topology change when using the exhaust protocol. In order to test this, we'll set the
 * maxAwaitTimeMS to much higher than the default (5 mins). This will allow us to assert that the
 * RSM receives the isMaster replies because of a topology change rather than maxAwaitTimeMS being
 * hit. A replica set node should send a response to the mongos as soon as it processes a topology
 * change, so "immediately"/"quickly" can vary - we specify 3 seconds in this test ('timeoutMS').
 *
 * @tags: [requires_streamable_rsm]
 */

// Checking UUID consistency and orphans involves talking to a shard node, which in this test is
// shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/rslib.js");

let overrideMaxAwaitTimeMS = {'mode': 'alwaysOn', 'data': {maxAwaitTimeMS: 5 * 60 * 1000}};
let st = new ShardingTest({
    mongos:
        {s0: {setParameter: "failpoint.overrideMaxAwaitTimeMS=" + tojson(overrideMaxAwaitTimeMS)}},
    shards: {rs0: {nodes: [{}, {}, {rsConfig: {priority: 0}}]}}
});

let timeoutMS = 3000;
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
st.rs0.waitForState(rsPrimary, ReplSetTest.State.SECONDARY);
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
st.rs0.start(rsPrimary);

rsPrimary = st.rs0.getPrimary();
st.rs0.getReplSetConfig().members.forEach(node => {
    if (node.priority != 0 && node.host != rsPrimary.host) {
        electableRsSecondary = st.rs0.nodes[node._id];
    }
});

// Terminate the primary and wait for the secondary to step up, trigger a topology change
jsTestLog("Terminating the primary.");
st.rs0.stop(rsPrimary, 15);
st.rs0.waitForState(electableRsSecondary, ReplSetTest.State.PRIMARY);
awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: false}, st.rs0, timeoutMS);
awaitRSClientHosts(
    mongos, {host: electableRsSecondary.name}, {ok: true, ismaster: true}, st.rs0, timeoutMS);

st.stop();
}());
