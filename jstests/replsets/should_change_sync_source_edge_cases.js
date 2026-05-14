/**
 * SERVER-126246 — companion matrix test for the
 * ReplicationCoordinatorImpl::shouldChangeSyncSource edge-case fix.
 *
 * The fix narrows the previously-unconditional return of
 *   ChangeSyncSourceAction::kStopSyncingAndEnqueueLastBatch
 * into a guarded return: when topology requests a source change but the
 * buffered last batch cannot be safely applied on a new sync source (the
 * scenario observed in BF-43158, where the commit timestamp of the last
 * batch precedes the new source's stable timestamp), the action must be
 * downgraded to kStopSyncingAndDropLastBatchIfPresent.
 *
 * This matrix exercises three rows of the decision FSM modelled in
 * src/mongo/tla_plus/Replication/SyncSourceSelection/SyncSourceSelection.tla:
 *
 *   row | entry point                     | topology | batch safe | expected
 *   ----+---------------------------------+----------+------------+----------
 *    1  | shouldChangeSyncSource          | wants    | yes        | enqueue
 *    2  | shouldChangeSyncSource          | wants    | no         | drop
 *    3  | shouldChangeSyncSourceOnError   | wants    | n/a        | drop
 *
 * Rows 4-6 (topology does not want change; ping-time wants change; both
 * decline → continue) are covered by existing tests in
 * jstests/replsets/sync_source_changes.js and the unit suite in
 * src/mongo/db/repl/replication_coordinator_impl_test.cpp.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {assertSyncSourceChangesTo} from "jstests/replsets/libs/sync_source.js";
import {reconfig} from "jstests/replsets/rslib.js";

// Replication verbosity 2 surfaces "Choosing new sync source" and the
// action-code log lines we assert on.
TestData["setParameters"]["logComponentVerbosity"]["replication"]["verbosity"] = 2;

const rst = new ReplSetTest({
    name: "should_change_sync_source_edge_cases",
    nodes: [
        {},
        {rsConfig: {priority: 0, votes: 0}},
        {rsConfig: {priority: 0, votes: 0}},
    ],
});
rst.startSet();
rst.initiate();
rst.awaitReplication();

const [primary, secondary, tertiary] = rst.nodes;

// Helper: scan log for a single occurrence of one of the action codes
// emitted by ReplicationCoordinatorImpl::shouldChangeSyncSource (verbosity
// 2 prints them).  Returns the action code observed, or null.
function lastSyncSourceActionFrom(conn) {
    const log = assert.commandWorked(conn.adminCommand({getLog: "global"})).log;
    let last = null;
    for (let i = log.length - 1; i >= 0; --i) {
        const line = log[i];
        if (line.indexOf("kStopSyncingAndEnqueueLastBatch") >= 0) {
            last = "kStopSyncingAndEnqueueLastBatch";
            break;
        }
        if (line.indexOf("kStopSyncingAndDropLastBatchIfPresent") >= 0) {
            last = "kStopSyncingAndDropLastBatchIfPresent";
            break;
        }
        if (line.indexOf("kContinueSyncing") >= 0) {
            last = "kContinueSyncing";
            break;
        }
    }
    return last;
}

// ---------------------------------------------------------------------------
// Row 1 — heartbeat path, batch safe ⇒ kStopSyncingAndEnqueueLastBatch.
// ---------------------------------------------------------------------------
jsTestLog("Row 1: enqueue-when-safe");
// Force the tertiary onto the secondary so we have a non-primary sync source
// we can disqualify cleanly.
{
    const sec = TestData.usePriorityPorts ? secondary.priorityHost : secondary.host;
    assert.commandWorked(tertiary.adminCommand({replSetSyncFrom: sec}));
}
assertSyncSourceChangesTo(rst, tertiary, secondary);
// Now reconfigure secondary to non-voter — this makes it ineligible.  The
// tertiary's buffered batch is safe (no timestamp pathology), so the
// expected action is enqueue.
{
    let cfg = rst.getReplSetConfigFromNode();
    cfg.members[1].votes = 0;
    cfg.members[1].priority = 0;
    reconfig(rst, cfg);
}
assertSyncSourceChangesTo(rst, tertiary, primary);
{
    const action = lastSyncSourceActionFrom(tertiary);
    assert.eq(
        action,
        "kStopSyncingAndEnqueueLastBatch",
        "row 1 expected enqueue when batch safe, observed: " + action,
    );
}

// ---------------------------------------------------------------------------
// Row 2 — heartbeat path, batch UNSAFE ⇒ kStopSyncingAndDropLastBatchIfPresent.
// We simulate the BF-43158 condition with the dedicated failpoint that the
// fix introduces: shouldChangeSyncSourceMarkLastBatchUnsafe causes the
// coordinator to treat the buffered batch as unsafe to enqueue.
// ---------------------------------------------------------------------------
jsTestLog("Row 2: drop-when-unsafe");
const markUnsafe = configureFailPoint(tertiary, "shouldChangeSyncSourceMarkLastBatchUnsafe");
{
    // Trigger a topology-wants-change event by reconfiguring the primary as
    // non-voter (forces the tertiary to switch).  We need a voting candidate
    // present, so flip secondary's votes back to 1 first.
    let cfg = rst.getReplSetConfigFromNode();
    cfg.members[1].votes = 1;
    cfg.members[1].priority = 1;
    reconfig(rst, cfg);
    rst.stepUp(secondary);
    cfg = rst.getReplSetConfigFromNode();
    cfg.members[0].votes = 0;
    cfg.members[0].priority = 0;
    reconfig(rst, cfg);
}
assertSyncSourceChangesTo(rst, tertiary, secondary);
{
    const action = lastSyncSourceActionFrom(tertiary);
    assert.eq(
        action,
        "kStopSyncingAndDropLastBatchIfPresent",
        "row 2 expected drop when batch unsafe (BF-43158), observed: " + action,
    );
}
markUnsafe.off();

// ---------------------------------------------------------------------------
// Row 3 — error path ⇒ kStopSyncingAndDropLastBatchIfPresent (never enqueue).
// shouldChangeSyncSourceOnError must never return enqueue; this is the
// ErrorPathNeverEnqueues invariant in the TLA+ spec.
// ---------------------------------------------------------------------------
jsTestLog("Row 3: error-path-never-enqueues");
// Force a fetcher error to drive the on-error code path.
const failFetcher = configureFailPoint(tertiary, "failfirstOplogEntryFetcherCallback");
{
    const newPrimary = rst.getPrimary();
    const newPrimaryHost = TestData.usePriorityPorts ? newPrimary.priorityHost : newPrimary.host;
    assert.commandWorked(tertiary.adminCommand({replSetSyncFrom: newPrimaryHost}));
}
failFetcher.wait();
// Allow the on-error decision to be logged.
assert.soon(() => {
    const action = lastSyncSourceActionFrom(tertiary);
    return action !== null && action !== "kStopSyncingAndEnqueueLastBatch";
}, "error path must not return enqueue (SERVER-126246)");
{
    const action = lastSyncSourceActionFrom(tertiary);
    assert.neq(
        action,
        "kStopSyncingAndEnqueueLastBatch",
        "row 3 error path must never enqueue, observed: " + action,
    );
}
failFetcher.off();

rst.stopSet();
