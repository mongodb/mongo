/**
 * SERVER-126510 reproducer.
 *
 * Forces a WriteConflictException (WCE) during the storage writes that
 * replSetInitiate performs on `local.system.replset` and the initiating oplog
 * entry, then confirms the command terminates cleanly rather than allowing the
 * WCE to propagate uncaught.
 *
 * The codepath under test is ReplicationCoordinatorExternalStateImpl::
 * initializeReplSetStorage (createOplog + the "initiate oplog entry"
 * writeConflictRetry block). Activating the WTWriteConflictException failpoint
 * at high probability across the per-node startup window exercises both the
 * already-retried writes and any sibling writes that were missing a retry
 * wrapper.
 *
 * Expected behaviour:
 *   - With the bug present, replSetInitiate fails with an uncaught
 *     WriteConflict error code (112) and / or the node aborts.
 *   - With the fix, all WCEs are caught by an enclosing writeConflictRetry
 *     loop and replSetInitiate succeeds (potentially after retries).
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

// WTWriteConflictException is best-effort and can fire on unrelated background
// writes; skip the dbhash check so the test is not penalised for those.
TestData.skipCheckDBHashes = true;

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: 1,
    nodeOptions: {
        setParameter: {
            traceWriteConflictExceptions: true,
            logComponentVerbosity: tojson({replication: 2, storage: 1}),
        },
    },
});

// startSet brings up mongod processes without initiating the set.
rst.startSet();

const node = rst.nodes[0];
const adminDB = node.getDB("admin");

// Activate the failpoint that injects synthetic WCEs into WiredTiger writes.
// activationProbability is intentionally high — the createOplog +
// "initiate oplog entry" path performs a small bounded number of writes, so a
// low probability frequently fails to fire at all. The fix path is required
// to absorb every WCE the failpoint produces.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "WTWriteConflictException",
    mode: {activationProbability: 0.5},
}));

const config = rst.getReplSetConfig();
jsTestLog("Calling replSetInitiate under WTWriteConflictException failpoint");

// Drive replSetInitiate directly. If a WCE escapes the
// initializeReplSetStorage / _runReplSetInitiate path uncaught, the command
// will fail with ErrorCodes::WriteConflict (112) or the node will terminate.
// A correctly wrapped path returns OK after retrying internally.
const initiateRes = adminDB.runCommand({replSetInitiate: config});

// Always disable the failpoint before any further assertions so we do not
// poison subsequent steady-state operations or the teardown path.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "WTWriteConflictException",
    mode: "off",
}));

// The whole point of SERVER-126510: replSetInitiate must NOT surface a
// WriteConflict (112) error to the caller. Any WCE on the initiate codepath
// has to be caught and retried by an enclosing writeConflictRetry block.
assert.neq(initiateRes.code,
           ErrorCodes.WriteConflict,
           "replSetInitiate leaked an uncaught WriteConflictException: " + tojson(initiateRes));

// Some other failure modes are legitimate (e.g. the failpoint perturbs a
// pre-initiate write enough that initiate retries succeed but a downstream
// invariant catches stale state). The bug is specifically the uncaught WCE,
// so we only assert on that. Surface other failures via tojson for triage
// rather than failing the test on them.
assert.commandWorked(initiateRes,
                     "replSetInitiate failed under WTWriteConflictException: " + tojson(initiateRes));

// If initiate succeeded, the node should reach PRIMARY normally.
rst.awaitNodesAgreeOnPrimary();
const primary = rst.getPrimary();
assert.eq(primary, node, "expected the only node to become primary after initiate");

// Sanity: write a doc and read it back to confirm the node is functional and
// that no torn state was left behind by the injected WCEs.
const testDB = primary.getDB("test");
assert.commandWorked(testDB.coll.insert({_id: 0, sentinel: jsTestName()}));
assert.eq(1, testDB.coll.find({_id: 0}).itcount(), "post-initiate write was lost");

rst.stopSet();
