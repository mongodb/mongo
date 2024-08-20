/**
 * This test ensures that a secondary that has gone "too stale" (i.e. cannot find another node with
 * a common oplog point) will transition to RECOVERING state, stay in RECOVERING after restart, and
 * transition back to SECONDARY once it finds a sync source with a common oplog point.
 *
 * Note: This test requires persistence in order for a restarted node with a stale oplog to stay in
 * the RECOVERING state. A restarted node with an ephemeral storage engine will not have an oplog
 * upon restart, so will immediately resync.
 *
 * @tags: [
 *   requires_persistence,
 * ]
 *
 * Replica Set Setup:
 *
 * Node 0 (PRIMARY)     : Small Oplog
 * Node 1 (SECONDARY)   : Large Oplog
 * Node 2 (SECONDARY)   : Small Oplog
 *
 * 1:  Insert one document on the primary (Node 0) and ensure it is replicated.
 * 2:  Stop node 2.
 * 3:  Wait until Node 2 is down.
 * 4:  Overflow the primary's oplog.
 * 5:  Stop Node 1 and restart Node 2.
 * 6:  Wait for Node 2 to transition to RECOVERING (it should be too stale).
 * 7:  Stop and restart Node 2.
 * 8:  Wait for Node 2 to transition to RECOVERING (its oplog should remain stale after restart).
 * 9:  Restart Node 1, which should have the full oplog history.
 * 10: Wait for Node 2 to leave RECOVERING and transition to SECONDARY.
 *
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {rollOver1MBOplog} from "jstests/replsets/libs/oplog_rollover_test.js";
import {getFirstOplogEntry} from "jstests/replsets/rslib.js";

/**
 * True if a node's entry in "members" has tooStale: true.
 */
function tooStale(conn) {
    return assert.commandWorked(conn.adminCommand("replSetGetStatus")).tooStale;
}

/**
 * Returns a node's current replica state.
 */
function myState(conn) {
    return assert.commandWorked(conn.adminCommand("replSetGetStatus")).myState;
}

var testName = "too_stale_secondary";

var smallOplogSizeMB = 1;
var bigOplogSizeMB = 1000;

// Node 0 is given a small oplog so we can overflow it. Node 1's large oplog allows it to
// store all entries comfortably without overflowing, so that Node 2 can eventually use it as
// a sync source after it goes too stale. Because this test overflows the oplog, a small
// syncdelay is chosen to frequently take checkpoints, allowing oplog truncation to proceed.
var replTest = new ReplSetTest({
    name: testName,
    nodes:
        [{oplogSize: smallOplogSizeMB}, {oplogSize: bigOplogSizeMB}, {oplogSize: smallOplogSizeMB}],
    nodeOptions: {
        syncdelay: 1,
        setParameter: {'failpoint.hangOplogCapMaintainerThread': tojson({mode: 'alwaysOn'})}
    },
});

var nodes = replTest.startSet();
replTest.initiate({
    _id: testName,
    members: [
        {_id: 0, host: nodes[0].host},
        {_id: 1, host: nodes[1].host, priority: 0},
        {_id: 2, host: nodes[2].host, priority: 0}
    ]
});

var dbName = testName;
var collName = "test";

jsTestLog("Wait for Node 0 to become the primary.");
replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);

var primary = replTest.getPrimary();
var primaryTestDB = primary.getDB(dbName);

jsTestLog("1: Insert one document on the primary (Node 0) and ensure it is replicated.");
assert.commandWorked(primaryTestDB[collName].insert({a: 1}, {writeConcern: {w: 3}}));
assert(!tooStale(replTest.nodes[2]));

jsTestLog("2: Stop Node 2.");
replTest.stop(2);

jsTestLog("3: Wait until Node 2 is down.");
replTest.waitForState(replTest.nodes[2], ReplSetTest.State.DOWN);

const firstOplogEntryPrimary = getFirstOplogEntry(primary);
const firstOplogEntryNode1 = getFirstOplogEntry(replTest.nodes[1]);

jsTestLog("4: Overflow the primary's oplog.");
rollOver1MBOplog(replTest);

// Test that oplog entry of the initial insert rolls over on the primary.
// Use assert.soon to wait for oplog cap maintainer thread to run.
assert.soon(() => {
    return getFirstOplogEntry(primary) != firstOplogEntryPrimary;
}, "Timeout waiting for oplog to roll over on primary");

// Make sure that Node 1's oplog didn't overflow. (This is best effort
// as this check could race with the maintainer thread running.)
assert.eq(firstOplogEntryNode1,
          getFirstOplogEntry(replTest.nodes[1]),
          "Node 1's oplog overflowed unexpectedly.");

jsTestLog("5: Stop Node 1 and restart Node 2.");
replTest.stop(1);
replTest.restart(2);

jsTestLog("6: Wait for Node 2 to transition to RECOVERING (it should be too stale).");
assert.soonNoExcept(() => tooStale(replTest.nodes[2]), "Waiting for Node 2 to become too stale");
// This checks the state as reported by the node itself.
assert.soon(() => myState(replTest.nodes[2]) === ReplSetTest.State.RECOVERING,
            "Waiting for Node 2 to transition to RECOVERING");
// This waits for the state as indicated by the primary node.
replTest.waitForState(replTest.nodes[2], ReplSetTest.State.RECOVERING);

jsTestLog("7: Stop and restart Node 2.");
replTest.stop(2);
replTest.restart(2, {
    // Set the failpoint to fail the transition to maintenance mode once. Make sure transitioning to
    // maintenance mode is resilient to errors (e.g. race with a concurrent election) and will
    // eventually succeed.
    setParameter: {'failpoint.setMaintenanceModeFailsWithNotSecondary': tojson({mode: {times: 1}})}
});

jsTestLog(
    "8: Wait for Node 2 to transition to RECOVERING (its oplog should remain stale after restart)");
assert.soonNoExcept(() => tooStale(replTest.nodes[2]), "Waiting for Node 2 to become too stale");
// This checks the state as reported by the node itself.
assert.soon(() => myState(replTest.nodes[2]) === ReplSetTest.State.RECOVERING,
            "Waiting for Node 2 to transition to RECOVERING");
// This waits for the state as indicated by the primary node.
replTest.waitForState(replTest.nodes[2], ReplSetTest.State.RECOVERING);

jsTestLog("9: Restart Node 1, which should have the full oplog history.");
replTest.restart(1);

jsTestLog("10: Wait for Node 2 to leave RECOVERING and transition to SECONDARY.");
assert.soonNoExcept(() => !tooStale(replTest.nodes[2]), "Waiting for Node 2 to exit too stale");
// This checks the state as reported by the node itself.
assert.soon(() => myState(replTest.nodes[2]) === ReplSetTest.State.SECONDARY,
            "Waiting for Node 2 to transition to SECONDARY");
// This waits for the state as indicated by the primary node.
replTest.waitForState(replTest.nodes[2], ReplSetTest.State.SECONDARY);

replTest.stopSet();
