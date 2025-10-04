/**
 * This test causes node 2 to enter rollback and then fail with a SocketException before updating
 * MinValid or altering durable state in any way. It will then choose a sync source from which it
 * is able to stitch the oplog and therefore doesn't need to roll back. Prior to SERVER-27282, the
 * node would be "stuck" with state=ROLLBACK while it was doing steady-state replication, with no
 * way to reach SECONDARY without restarting the process.
 *
 * @tags: [requires_fcv_53]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {waitForState} from "jstests/replsets/rslib.js";

let collName = "test.coll";
let counter = 0;

let rst = new ReplSetTest({
    name: "rollback_with_socket_error_then_steady_state",
    nodes: [
        // Primary flops between nodes 0 and 1.
        {},
        {},
        // Node 2 is the node under test.
        {rsConfig: {priority: 0}},
        // Arbiters to sway elections.
        {rsConfig: {arbiterOnly: true}},
        {rsConfig: {arbiterOnly: true}},
    ],
    useBridge: true,
});
let nodes = rst.startSet({setParameter: {allowMultipleArbiters: true}});
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

// The default WC is majority and stopServerReplication could prevent satisfying any majority
// writes.
assert.commandWorked(
    rst.getPrimary().adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);
rst.awaitReplication();

function stepUp(rst, node) {
    let primary = rst.getPrimary();
    if (primary != node) {
        assert.commandWorked(primary.adminCommand({replSetStepDown: 1, force: true}));
    }
    waitForState(node, ReplSetTest.State.PRIMARY);
}

jsTestLog("Make sure node 0 is primary.");
stepUp(rst, nodes[0]);
assert.eq(nodes[0], rst.getPrimary());
// Wait for all data bearing nodes to get up to date.
assert.commandWorked(
    nodes[0]
        .getCollection(collName)
        .insert({a: counter++}, {writeConcern: {w: 3, wtimeout: ReplSetTest.kDefaultTimeoutMS}}),
);

jsTestLog("Create two partitions: [1] and [0,2,3,4].");
nodes[1].disconnect(nodes[0]);
nodes[1].disconnect(nodes[2]);
nodes[1].disconnect(nodes[3]);
nodes[1].disconnect(nodes[4]);

jsTestLog("Do a write that is replicated to [0,2,3,4].");
assert.commandWorked(
    nodes[0]
        .getCollection(collName)
        .insert({a: counter++}, {writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMS}}),
);

jsTestLog("Repartition to: [0,2] and [1,3,4].");
nodes[1].reconnect(nodes[3]);
nodes[1].reconnect(nodes[4]);
nodes[3].disconnect(nodes[0]);
nodes[3].disconnect(nodes[2]);
nodes[4].disconnect(nodes[0]);
nodes[4].disconnect(nodes[2]);

jsTestLog("Ensure that 0 steps down and that 1 becomes primary.");
waitForState(nodes[0], ReplSetTest.State.SECONDARY);
waitForState(nodes[1], ReplSetTest.State.PRIMARY);
assert.eq(nodes[1], rst.getPrimary());

jsTestLog("Do a write to node 1 on the [1,3,4] side of the partition.");
assert.commandWorked(nodes[1].getCollection(collName).insert({a: counter++}));

// Turn on failpoint on node 2 to pause rollback before doing anything.
let failPoint = configureFailPoint(nodes[2], "rollbackHangBeforeStart");

jsTestLog("Repartition to: [0] and [1,2,3,4].");
nodes[2].disconnect(nodes[0]);
nodes[2].reconnect(nodes[1]);
nodes[2].reconnect(nodes[3]);
nodes[2].reconnect(nodes[4]);

jsTestLog("Wait for node 2 to decide to go into ROLLBACK and start syncing from node 1.");
// Since nodes 1 and 2 have now diverged, node 2 should go into rollback. The failpoint will
// stop it from actually transitioning to rollback, so the wait bellow will ensure that we
// have decided to rollback, but haven't actually started yet.
rst.awaitSyncSource(nodes[2], nodes[1]);

jsTestLog("Wait for failpoint on node 2 to pause rollback before it starts");
// Wait for fail point message to be logged.
failPoint.wait();

jsTestLog("Repartition to: [1] and [0,2,3,4].");
nodes[1].disconnect(nodes[3]);
nodes[1].disconnect(nodes[4]);
nodes[2].disconnect(nodes[1]);
nodes[2].reconnect(nodes[0]);
nodes[3].reconnect(nodes[0]);
nodes[3].reconnect(nodes[2]);
nodes[4].reconnect(nodes[0]);
nodes[4].reconnect(nodes[2]);

// Turn off failpoint on node 2 to allow rollback against node 1 to fail with a network error.
failPoint.off();

// Make node 0 ahead of node 2 again so node 2 will pick it as a sync source.

jsTestLog("waiting for node 0 to be primary");
waitForState(nodes[1], ReplSetTest.State.SECONDARY);
waitForState(nodes[0], ReplSetTest.State.PRIMARY);
assert.eq(nodes[0], rst.getPrimary());

jsTestLog("w:2 write to node 0 (replicated to node 2)");
assert.commandWorked(
    nodes[0]
        .getCollection(collName)
        .insert({a: counter++}, {writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMS}}),
);

// At this point node 2 has failed rollback before making any durable changes, including writing
// to minValid. That means that it is free to pick any sync source and will pick node 0 where it
// can pick up where it left off without rolling back. Ensure that it is able to reach SECONDARY
// and doesn't do steady-state replication in ROLLBACK state.
jsTestLog("Wait for node 2 to go into SECONDARY");
assert.neq(
    nodes[2].adminCommand("replSetGetStatus").myState,
    ReplSetTest.State.ROLLBACK,
    "node 2 is doing steady-state replication with state=ROLLBACK!",
);
waitForState(nodes[2], ReplSetTest.State.SECONDARY);

// Re-connect all nodes and await secondary nodes so we can check data consistency.
nodes[1].reconnect([nodes[0], nodes[2], nodes[3], nodes[4]]);
rst.awaitSecondaryNodes();

// Verify data consistency between nodes.
rst.checkReplicatedDataHashes();
rst.checkOplogs();
rst.stopSet();
