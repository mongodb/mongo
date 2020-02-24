/**
 * In a 4-node set, verify that two diverging non-force replica set reconfigs
 * are not allowed to succeed. Diverging reconfigs contain non-overlapping quorums. For example,
 * C1: {n1,n2,n3}
 * C2: {n1,n3,n4}
 * The C1 quorum {n1,n2} and the C2 quorum {n3,n4} do not overlap.
 *
 * 1. Node1 is the initial primary.
 * 2. Disconnect node4 from all three other nodes.
 * 3. Step down node1 and step up node2.
 * 4. Disconnect the current primary, node2, from all other nodes.
 * 5. Issue a reconfig to node2 that removes node4.
 * 6. Reconnect node4 to the current secondaries, node1 and node3.
 * 7. Step up node3, which creates a two primary scenario.
 * 8. Issue a reconfig to node3 that removes node2. We now have diverging configs
 *   from two different primaries.
 * 9. Reconnect node2 to the rest of the set and verify that its reconfig fails.
 *
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";
load("jstests/libs/parallel_shell_helpers.js");
load('jstests/libs/test_background_ops.js');
load("jstests/replsets/rslib.js");
load('jstests/aggregation/extras/utils.js');

const dbName = "test";
const collName = "coll";
let rst = new ReplSetTest({nodes: 4, useBridge: true});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const node1 = rst.getPrimary();
const secondaries = rst.getSecondaries();
const node2 = secondaries[0];
const node3 = secondaries[1];
const node4 = secondaries[2];
const coll = node1.getDB(dbName)[collName];

// Partition the 4th node.
node4.disconnect([node1, node2, node3]);

jsTestLog("Current replica set topology: [Primary-Secondary-Secondary] [Secondary]");
assert.commandWorked(node1.adminCommand({replSetStepDown: 120}));
// Step up a new primary in the partitioned repl set.
assert.commandWorked(node2.adminCommand({replSetStepUp: 1}));

// Wait until the config has been committed.
assert.soon(() => isConfigCommitted(rst.getPrimary()));
// The quorum check places stricter bounds on the safe reconfig
// protocol and won't allow this specific scenario of diverging configs
// to happen. However, it's still worth testing the original reconfig
// protocol that omitted the check for correctness.
configureFailPoint(rst.getPrimary(), "omitConfigQuorumCheck");

// Reconfig to remove the 4th node.
const C1 = Object.assign({}, rst.getReplSetConfigFromNode());
C1.members = C1.members.slice(0, 3);  // Remove the last node.
C1.version++;

jsTestLog("Disconnecting the primary from other nodes");
assert.eq(rst.getPrimary(), node2);
node2.disconnect([node1, node3, node4]);
jsTestLog("Current replica set topology: [Primary] [Secondary-Secondary] [Secondary]");
// Create parallel shell to execute reconfig on partitioned primary.
// This reconfig will succeed due to the omission of the quorum check, but
// will not get propagated.
startParallelShell(funWithArgs(function(config) {
                       assert.commandWorked(db.getMongo().adminCommand({replSetReconfig: config}));
                   }, C1), node2.port);

// Reconnect the 4th node to the secondaries.
node4.reconnect([node1, node3]);
node3.adminCommand({replSetStepUp: 1});
rst.awaitNodesAgreeOnPrimary(rst.kDefaultTimeoutMS, [node1, node3, node4]);
jsTestLog("Current replica set topology: [Primary-Secondary-Secondary] [Primary]");
assert.soon(function() {
    return isConfigCommitted(node3);
});

// Reconfig to remove a secondary. We need to specify the node to get the original
// config from as there are two primaries, node2 and node3, in the replset.
let C2 = Object.assign({}, rst.getReplSetConfigFromNode(2));
const removedSecondary = C2.members.splice(0, 1);
C2.version++;
assert.commandWorked(node3.adminCommand({replSetReconfig: C2}));
assert.soon(() => isConfigCommitted(node3));

// Reconnect partitioned node to the other nodes.
node2.reconnect([node3, node4]);
// The newly connected node will receive a heartbeat with a higher term, and
// step down from being primary. The reconfig command issued to this node will fail.
rst.waitForState(node2, ReplSetTest.State.SECONDARY);

// Make sure the newly connected secondary has updated its config.
assert.soon(function() {
    const node2TermUpdated = bsonWoCompare(node2.adminCommand({replSetGetStatus: 1}).term,
                                           node3.adminCommand({replSetGetStatus: 1}).term) == 0;
    const node2ConfigTermUpdated =
        node2.adminCommand({replSetGetStatus: 1}).members[1].configTerm ==
        node3.adminCommand({replSetGetStatus: 1}).members[2].configTerm;
    const node2ConfigVersionUpdated =
        node2.adminCommand({replSetGetStatus: 1}).members[1].configVersion ==
        node3.adminCommand({replSetGetStatus: 1}).members[2].configVersion;
    return node2TermUpdated && node2ConfigTermUpdated && node2ConfigVersionUpdated;
});

// Reconnect the 4th node to return to a stable state.
let C3 = Object.assign({}, rst.getReplSetConfigFromNode(2));
C3.members.push(removedSecondary[0]);
C3.version++;

node1.reconnect([node2, node3, node4]);
assert.commandWorked(node3.adminCommand({replSetReconfig: C3}));
assert.soon(function() {
    return isConfigCommitted(node3);
});
rst.awaitNodesAgreeOnPrimary();
rst.awaitNodesAgreeOnConfigVersion();
rst.stopSet();
}());
