/**
 * In a 4-node set, verify that two diverging non-force replica set reconfigs
 * are not allowed to succeed. Diverging reconfigs contain non-overlapping quorums. For example,
 * C1: {n1,n2,n3}
 * C2: {n1,n3,n4}
 * The C1 quorum {n1,n2} and the C2 quorum {n3,n4} do not overlap.
 *
 * 1. Node0 is the initial primary.
 * 2. Disconnect node0 from all other nodes.
 * 3. Issue a reconfig to node0 that removes node3.
 * 4. Step up node1, which creates a two primary scenario.
 * 5. Issue a reconfig to node1 that removes node2. We now have diverging configs
 *   from two different primaries.
 * 6. Reconnect node0 to the rest of the set and verify that its reconfig fails.
 */
(function() {
"use strict";
load("jstests/libs/parallel_shell_helpers.js");
load('jstests/libs/test_background_ops.js');
load("jstests/replsets/rslib.js");
load('jstests/aggregation/extras/utils.js');
load("jstests/libs/fail_point_util.js");

let rst = new ReplSetTest({nodes: 4, useBridge: true});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const node0 = rst.getPrimary();
const [node1, node2, node3] = rst.getSecondaries();
jsTestLog("Current replica set topology: [node0 (Primary), node1, node2, node3]");

// The quorum check places stricter bounds on the safe reconfig
// protocol and won't allow this specific scenario of diverging configs
// to happen. However, it's still worth testing the original reconfig
// protocol that omitted the check for correctness.
configureFailPoint(rst.getPrimary(), "omitConfigQuorumCheck");

// Reconfig to remove the node3. The new config, C1, is now {node0, node1, node2}.
const C1 = Object.assign({}, rst.getReplSetConfigFromNode());
C1.members = C1.members.slice(0, 3);  // Remove the last node.
// Increase the C1 version by a high number to ensure the following config
// C2 will win the propagation by having a higher term.
C1.version = C1.version + 1000;
rst.waitForConfigReplication(node0);
rst.awaitReplication();

jsTestLog("Disconnecting the primary from other nodes");
assert.eq(rst.getPrimary(), node0);
node0.disconnect([node1, node2, node3]);
jsTestLog("Current replica set topology: [node0 (Primary)] [node1, node2, node3]");
// Create parallel shell to execute reconfig on partitioned primary.
// This reconfig will not get propagated.
const parallelShell = startParallelShell(
    funWithArgs(function(config) {
        const res = db.getMongo().adminCommand({replSetReconfig: config});
        assert(ErrorCodes.isNotPrimaryError(res.code), "Reconfig C1 should fail" + tojson(res));
    }, C1), node0.port);

assert.commandWorked(node1.adminCommand({replSetStepUp: 1}));
rst.awaitNodesAgreeOnPrimary(rst.kDefaultTimeoutMS, [node1, node2, node3], node1);
jsTestLog("Current replica set topology: [node0 (Primary)] [node1 (Primary), node2, node3]");
assert.soon(() => node1.getDB('admin').runCommand({hello: 1}).isWritablePrimary);
assert.soon(() => isConfigCommitted(node1));

// Reconfig to remove a secondary. We need to specify the node to get the original
// config from as there are two primaries, node0 and node1, in the replset.
// The new config is now {node0, node1, node3}.
let C2 = Object.assign({}, rst.getReplSetConfigFromNode(1));
const removedSecondary = C2.members.splice(2, 1);
C2.version++;
assert.commandWorked(node1.adminCommand({replSetReconfig: C2}));
assert.soon(() => isConfigCommitted(node1));

// Reconnect the partitioned primary, node0, to the other nodes.
node0.reconnect([node1, node2, node3]);
// The newly connected node will receive a heartbeat with a higher term, and
// step down from being primary. The reconfig command issued to this node, C1, will fail.
rst.waitForState(node0, ReplSetTest.State.SECONDARY);
rst.awaitNodesAgreeOnPrimary(rst.kDefaultTimeoutMS, [node0, node1, node3], node1);
rst.waitForConfigReplication(node1);
assert.eq(C2, rst.getReplSetConfigFromNode());

// The new config is now {node0, node1, node2, node3}.
let C3 = Object.assign({}, rst.getReplSetConfigFromNode(1));
C3.members.push(removedSecondary[0]);
C3.version++;

assert.commandWorked(node1.adminCommand({replSetReconfig: C3}));
assert.soon(() => isConfigCommitted(node1));
rst.awaitNodesAgreeOnPrimary();
parallelShell();
rst.stopSet();
}());
