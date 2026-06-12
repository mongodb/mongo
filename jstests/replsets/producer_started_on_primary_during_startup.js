/**
 * Reproduces the stepup-during-startup race where a node wins an election in the window between
 * finishing oplog recovery and starting steady-state replication. In that case the oplog producer
 * (BackgroundSync) is created *after* the node is already primary, so the drain's stopProducer() was
 * a no-op (the producer did not yet exist). The freshly started producer then re-fetches the new
 * primary's own step-up no-op from a secondary and the oplog writer tries to rewrite it at a
 * timestamp at or below the stable timestamp, tripping a fatal WiredTiger invariant:
 *
 *   __txn_validate_commit_timestamp: commit timestamp (t, i) must be after the stable timestamp ...
 *   Fatal assertion 4017301
 *
 * The ordering is forced deterministically with the test-only failpoint
 * 'hangBeforeStartingSteadyStateReplicationAfterRecovery', which lets the restarting node reach
 * SECONDARY (and thus be electable) but pauses before creating/starting the producer.
 *
 * Without the fix this deterministically trips fassert 4017301; with the producer-stop guard the
 * node instead becomes a healthy writable primary, which is what this test asserts.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_persistence,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 2,
    // Mirror the BF: catch-up disabled, so a freshly elected primary immediately drains and writes
    // its step-up no-op.
    settings: {catchUpTimeoutMillis: 0},
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const secondary = rst.getSecondary();

// Make sure both nodes are caught up before we restart the secondary.
assert.commandWorked(primary.getDB("test").c.insert({_id: 0}, {writeConcern: {w: 2}}));
rst.awaitReplication();

const initialTerm = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1})).term;

jsTestLog("Restarting the secondary with the steady-state-replication-after-recovery failpoint.");
const restartedNodeId = rst.getNodeId(secondary);
rst.stop(restartedNodeId, undefined /* signal */, {} /* opts */, {forRestart: true});
const restarted = rst.start(
    restartedNodeId,
    {
        setParameter:
            "failpoint.hangBeforeStartingSteadyStateReplicationAfterRecovery={'mode':'alwaysOn'}",
    },
    true /* restart */,
);

// The node should reach SECONDARY (the failpoint runs finishRecoveryIfEligible) and then pause
// before starting steady-state replication, i.e. before creating the oplog producer.
jsTestLog("Waiting for the restarted node to reach SECONDARY while paused before steady state.");
rst.awaitSecondaryNodes(null, [restarted]);

// Elect the restarted node so that steady-state replication (and the producer) will be created
// while it is already primary. Run stepUp from a parallel shell because, while the producer is not
// yet started, completing the step-up drain blocks until we release the failpoint.
//
// Retry the stepUp until the election wins: a single attempt can fail when the current primary's
// LastVote write is interrupted by its own concurrent stepdown (triggered by the higher term we
// propose) -- "InterruptedDueToReplStateChange" -- which is more likely on slow/sanitizer machines.
// Once the old primary has stepped down, a retry gets its vote and wins. Exceptions/non-ok results
// (including the connection drop if the node ever crashed) are swallowed and retried.
jsTestLog("Stepping up the restarted node while it is paused before steady-state replication.");
const stepUpShell = startParallelShell(() => {
    assert.soonNoExcept(() => {
        return db.getSiblingDB("admin").runCommand({replSetStepUp: 1}).ok;
    }, "failed to step up the restarted node");
}, restarted.port);

// Wait until the restarted node considers itself primary (it will be stuck in drain until released).
assert.soon(() => {
    const res = restarted.adminCommand({replSetGetStatus: 1});
    return res.ok && res.myState === ReplSetTest.State.PRIMARY && res.term > initialTerm;
}, "restarted node never became primary in a new term while paused");

jsTestLog(
    "Releasing the failpoint; steady-state replication (the producer) now starts on a " +
        "node that is already primary.",
);
assert.commandWorked(
    restarted.adminCommand({
        configureFailPoint: "hangBeforeStartingSteadyStateReplicationAfterRecovery",
        mode: "off",
    }),
);

// Without the fix one of two things happens: the step-up drain never completes (the oplog buffer
// was created after the node entered drain mode, so the writer blocks forever), leaving a
// non-writable primary; or, under the right async-writer timing, the producer re-fetches the
// primary's own step-up no-op and the oplog writer fassert()s (4017301). With the fix the node
// becomes a healthy writable primary. Retry the write until it succeeds: it returns
// NotWritablePrimary until the drain completes, throws if the node crashed, and succeeds once the
// node is a healthy writable primary.
jsTest.log.info("Verifying the restarted node becomes a healthy writable primary after release.");
assert.soonNoExcept(
    () => {
        const res = restarted
            .getDB("test")
            .runCommand({insert: "c", documents: [{from: "restarted"}]});
        return res.ok === 1;
    },
    "restarted node did not become a healthy writable primary after release",
    60 * 1000,
);

stepUpShell();

// Defense in depth: the fatal WiredTiger stable-timestamp invariant (fassert 4017301) must not have
// been hit.
const logContents = rawMongoProgramOutput(".*");
assert(
    !/Fatal assertion.*4017301/.test(logContents) &&
        !/must be after the stable timestamp/.test(logContents),
    "Restarted node hit the WiredTiger stable-timestamp fatal invariant (4017301)",
);

rst.stopSet();
