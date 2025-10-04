// Test to ensure that a catchup takeover happens when the primary is lagged.
// Make sure that when two nodes are more caught up than the primary,
// the most up-to-date node becomes the primary.

// 5-node replica set
// Start replica set. Ensure that node 0 becomes primary.
// Stop replication for some nodes and have the primary write something.
// Stop replication for an up-to-date node and have the primary write something.
// Now the primary is most-up-to-date and another node is more up-to-date than others.
// Make a lagged node the next primary.
// Confirm that the most up-to-date node becomes primary.
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {
    verifyCatchUpConclusionReason,
    verifyServerStatusElectionReasonCounterChange,
} from "jstests/replsets/libs/election_metrics.js";

let name = "catchup_takeover_two_nodes_ahead";
let replSet = new ReplSetTest({name: name, nodes: 5});
let nodes = replSet.startSet();
let config = replSet.getReplSetConfig();
// Prevent nodes from syncing from other secondaries.
config.settings = {
    chainingAllowed: false,
};
replSet.initiate(config);
replSet.awaitReplication();

// Write something so that nodes 0 and 1 are ahead.
stopServerReplication(nodes.slice(2, 5));
const primary = replSet.getPrimary();
let writeConcern = {writeConcern: {w: 2, wtimeout: replSet.timeoutMS}};
assert.commandWorked(primary.getDB(name).bar.insert({x: 100}, writeConcern));

// Write something so that node 0 is ahead of node 1.
stopServerReplication(nodes[1]);
writeConcern = {
    writeConcern: {w: 1, wtimeout: replSet.timeoutMS},
};
assert.commandWorked(primary.getDB(name).bar.insert({y: 100}, writeConcern));

const initialPrimaryStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
const initialNode2Status = assert.commandWorked(nodes[2].adminCommand({serverStatus: 1}));

// Step up one of the lagged nodes.
assert.commandWorked(nodes[2].adminCommand({replSetStepUp: 1}));
replSet.awaitNodesAgreeOnPrimary();
assert.eq(
    ReplSetTest.State.PRIMARY,
    assert.commandWorked(nodes[2].adminCommand("replSetGetStatus")).myState,
    nodes[2].host + " was not primary after step-up",
);
jsTestLog("node 2 is now primary, but cannot accept writes");

// Make sure that node 2 cannot write anything. Because it is lagged and replication
// has been stopped, it shouldn't be able to become primary.
assert.commandFailedWithCode(nodes[2].getDB(name).bar.insert({z: 100}, writeConcern), ErrorCodes.NotWritablePrimary);

// Confirm that the most up-to-date node becomes primary after the default catchup delay.
replSet.waitForState(0, ReplSetTest.State.PRIMARY, 60 * 1000);

// Check that both the 'called' and 'successful' fields of the 'catchUpTakeover' election reason
// counter have been incremented in serverStatus.
const newPrimaryStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
verifyServerStatusElectionReasonCounterChange(
    initialPrimaryStatus.electionMetrics,
    newPrimaryStatus.electionMetrics,
    "catchUpTakeover",
    1,
);

// Wait until the old primary steps down.
replSet.awaitSecondaryNodes(replSet.timeoutMS, [nodes[2]]);

// Check that the 'numCatchUpsFailedWithNewTerm' field has been incremented in serverStatus, and
// that none of the other reasons for catchup concluding has been incremented.
const newNode2Status = assert.commandWorked(nodes[2].adminCommand({serverStatus: 1}));
verifyCatchUpConclusionReason(
    initialNode2Status.electionMetrics,
    newNode2Status.electionMetrics,
    "numCatchUpsFailedWithNewTerm",
);

// Let the nodes catchup.
restartServerReplication(nodes.slice(1, 5));
replSet.stopSet();
