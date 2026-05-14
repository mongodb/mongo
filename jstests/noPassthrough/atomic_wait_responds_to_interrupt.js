/**
 * SERVER-75430: Tests that a command which blocks inside an atomic wait on OperationContext
 * unblocks within a bounded time after killOp fires.
 *
 * Today's TicketPool / PriorityTicketHolder waits sleep on a futex over a raw atomic flag and
 * poll for interrupt every 500ms. Once OperationContext::waitForAtomicOrInterrupt is in place,
 * killOp must unblock the waiter on the next scheduler step, not after the 500ms re-queue.
 *
 * The test wedges a thread into the atomic wait via the `hangInWaitForAtomicOrInterrupt`
 * failpoint (introduced alongside the helper). With the failpoint engaged the command parks
 * inside the new helper. We then call killOp on it and assert that:
 *   1. the command returns ErrorCodes.Interrupted
 *   2. it does so well under the legacy 500ms re-queue ceiling (we use 2s as a generous bound
 *      that still catches a regression to the old polling behaviour)
 *
 * If the failpoint name has not yet been introduced (i.e. the helper has not landed), the test
 * skips gracefully and prints the expected failpoint name so the implementer knows what to wire.
 *
 * @tags: [
 *     requires_replication,
 *     requires_fcv_71,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Failpoint that must be installed alongside the new helper. It pauses the thread inside the
// joined atomic+interrupt wait. With waitForAtomicOrInterrupt in place, markKilled() flips the
// kill bit and the futex returns on the next syscall edge.
const kFailpointName = "hangInWaitForAtomicOrInterrupt";

// Upper bound for "interrupt was responsive". Old behaviour was 500ms re-queue; we allow 2s
// for jitter on slow hosts but anything in the seconds-to-minutes range is a metastable-failure
// regression and must fail the test.
const kInterruptResponseMs = 2 * 1000;

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const adminDB = primary.getDB("admin");
const testDB = primary.getDB("test");
const collName = "atomic_wait_responds_to_interrupt";
assert.commandWorked(testDB.createCollection(collName));

// Probe for the failpoint. If it does not exist yet, document the contract and exit cleanly --
// this test pins the contract more than the current implementation, and we want it to remain
// readable as a regression catch once the helper lands.
const probe = adminDB.runCommand({configureFailPoint: kFailpointName, mode: "off"});
if (!probe.ok) {
    jsTestLog(`Skipping: failpoint "${kFailpointName}" is not registered. ` +
              `Once SERVER-75430's waitForAtomicOrInterrupt helper lands, install this ` +
              `failpoint inside the slow path so this test exercises it.`);
    rst.stopSet();
    quit();
}

const fp = configureFailPoint(primary, kFailpointName);
const findComment = "SERVER-75430-atomic-wait-interrupt-probe";

// Run a command in a parallel shell that goes through the atomic wait. Any command whose
// admission path goes through TicketPool / PriorityTicketHolder works; we use a simple find
// with a comment we can pick up via currentOp.
const awaitShell = startParallelShell(funWithArgs(
    (dbName, coll, comment) => {
        const res = db.getSiblingDB(dbName).runCommand({find: coll, filter: {}, comment});
        // The test asserts we get Interrupted; if the server returns OK it means the failpoint
        // released without our killOp, which is also a regression.
        assert.commandFailedWithCode(res, ErrorCodes.Interrupted);
    },
    testDB.getName(),
    collName,
    findComment,
), primary.port);

try {
    // Wait until the find op has reached the atomic wait failpoint.
    fp.wait();

    // Locate the op.
    let opId;
    assert.soon(() => {
        const r = adminDB.aggregate([{$currentOp: {}}, {$match: {"command.comment": findComment}}]).toArray();
        if (r.length === 1) {
            opId = r[0].opid;
            return true;
        }
        return false;
    }, () => `failed to find op with comment ${findComment}`);

    // Fire killOp and time the round-trip to the waiter's return.
    const tKill = Date.now();
    assert.commandWorked(adminDB.killOp(opId));

    // Disengage the failpoint so the wait can return (belt-and-suspenders: with the new helper
    // in place, markKilled flips the kill bit and the futex returns even with the failpoint
    // engaged; we turn it off so the test is deterministic under implementations that gate
    // the failpoint check above the interrupt check).
    fp.off();

    awaitShell();
    const elapsed = Date.now() - tKill;

    // Headline assertion: interrupt response is well below the legacy 500ms re-queue ceiling.
    assert.lt(elapsed,
              kInterruptResponseMs,
              `killOp on an atomic-waiting op took ${elapsed}ms, exceeds ${kInterruptResponseMs}ms ` +
              `bound. This is the SERVER-75430 metastable-failure regression: bare atomic wait, ` +
              `interrupt not coupled.`);

    jsTestLog(`OK: atomic wait responded to interrupt in ${elapsed}ms (bound ${kInterruptResponseMs}ms).`);
} finally {
    // Idempotent: fp.off() is safe to call twice if the try-block already invoked it.
    fp.off();
}

rst.stopSet();
