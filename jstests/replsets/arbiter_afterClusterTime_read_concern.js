/**
 * Regression test for a crash where sending a command with
 * readConcern:{afterClusterTime: ...} to an arbiter aborted the process.
 *
 * An arbiter's VectorClock is disabled in onBecomeArbiter(), so once disabled its
 * clusterTime never advances past the uninitialized value (Timestamp(0, 1)). The
 * afterClusterTime handling in waitForReadConcernImpl assumed an uninitialized
 * clusterTime only occurs in STARTUP/STARTUP2 and would invariant-fail when the
 * node was instead in state ARBITER. This is reachable without authentication
 * because listCommands does not require auth and allows afterClusterTime.
 *
 * We reach the uninitialized-clock state by restarting the arbiter. On a persistent
 * storage engine the arbiter reads its config from disk and onBecomeArbiter() disables
 * the VectorClock during config load, before any heartbeat can gossip in a clusterTime,
 * so the clock deterministically stays at Timestamp(0, 1).
 *
 * This requires persistence: on an in-memory storage engine the restarted arbiter has
 * no local config and must re-fetch it from the primary via a heartbeat, which gossips
 * in a clusterTime before the clock is disabled, leaving the clock initialized.
 *
 * @tags: [requires_replication, requires_persistence]
 */
const name = "arbiter_afterClusterTime_read_concern";
const replTest = new ReplSetTest({name: name, nodes: 2});
const nodes = replTest.nodeList();

replTest.startSet();
replTest.initiate({
    _id: name,
    members: [
        {"_id": 0, "host": nodes[0]},
        {"_id": 1, "host": nodes[1], arbiterOnly: true},
    ],
});

const arbiterId = replTest.getNodeId(replTest.getArbiter());

// Stop the primary so the arbiter cannot get a heartbeat before it disables its
// vector clock causing it to be uninitialized.
replTest.stop(replTest.getNodeId(replTest.getPrimary()));

// Restart the arbiter to reach the uninitialized-clock state. On config load
// from disk we disable the vector clock before it has a timestamp set. A node
// that is already running and becomes arbiter has its vector clock disabled at
// its current timestamp, which is not uninitialized.
const arbiter = replTest.restart(arbiterId);

// Confirm the node has come back up as an arbiter before exercising the read.
replTest.waitForState(arbiter, ReplSetTest.State.ARBITER);

// Return a graceful error (NotPrimaryOrSecondary) because the arbiter cannot
// service the requested clusterTime.
const res = arbiter.getDB("admin").runCommand({
    listCommands: 1,
    readConcern: {afterClusterTime: Timestamp(1, 1)},
});

assert.commandFailedWithCode(res, ErrorCodes.NotPrimaryOrSecondary, tojson(res));

// The error reports the clusterTime the node actually observed. Confirm it is the
// uninitialized value Timestamp(0, 1) like we expect.
assert(
    res.errmsg.includes("current clusterTime: { ts: Timestamp(0, 1) }"),
    "error did not report an uninitialized current clusterTime: " + tojson(res),
);

// The arbiter must still be up and responsive afterwards.
assert.commandWorked(arbiter.getDB("admin").runCommand({ping: 1}));

replTest.stopSet();
