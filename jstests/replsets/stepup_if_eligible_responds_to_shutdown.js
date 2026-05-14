/**
 * SERVER-125948: Ensures that mongod can shut down even when a stepUpIfEligible call is blocked
 * mid-flight. The function updates global state and gates other operations, so it must respect
 * the OperationContext interrupt that shutdown installs.
 *
 * Repro shape:
 *   1. Bring up a 3-node replica set.
 *   2. Freeze a step-up in the secondary by configuring a failpoint that pauses execution
 *      inside stepUpIfEligible (post-entry, pre-completion).
 *   3. Kick off replSetStepUp in a parallel shell so the failpoint engages.
 *   4. Send shutdown to the same node and assert mongod exits within 30s rather than wedging
 *      behind the failpoint until the test harness's idle timeout fires (~7 min, see BF-43105).
 *
 * Acceptance: shutdown returns / process exits within kShutdownDeadlineMs. If stepUpIfEligible
 * is not interruptible, the node hangs past the failpoint indefinitely and this test times out.
 *
 * @tags: [
 *   requires_replication,
 *   requires_majority_read_concern,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kShutdownDeadlineMs = 30 * 1000;

const replTest = new ReplSetTest({name: jsTestName(), nodes: 3});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const secondaries = replTest.getSecondaries();
const target = secondaries[0];

assert.commandWorked(primary.getDB("test").c.insert({_id: 0}, {writeConcern: {w: 3}}));

// Freeze step-up after the function entered but before it returns. The failpoint name follows the
// pattern of other repl coordinator failpoints (e.g. stepdownHangBeforeRSTLEnqueue). If this
// failpoint is renamed during the fix, update this string -- the bug repro is the same.
jsTestLog("Installing hangInStepUpIfEligible failpoint on the target secondary.");
const fp = configureFailPoint(target, "hangInStepUpIfEligible");

jsTestLog("Kicking off replSetStepUp on the target secondary in a parallel shell.");
const stepUpShell = startParallelShell(function () {
    // Best-effort: we don't care if this command returns success, fails, or is interrupted by
    // the upcoming shutdown -- only that the process exits.
    try {
        db.adminCommand({replSetStepUp: 1, skipDryRun: true});
    } catch (e) {
        jsTestLog("replSetStepUp shell exited with: " + tojson(e));
    }
}, target.port);

jsTestLog("Waiting for the target to enter stepUpIfEligible and hit the failpoint.");
fp.wait();

const shutdownStart = Date.now();
jsTestLog("Sending shutdown to the wedged target. mongod must exit within " +
          kShutdownDeadlineMs + "ms despite the failpoint still being active.");

// Shutdown in a parallel shell so the main thread can enforce the deadline.
const shutdownShell = startParallelShell(function () {
    try {
        db.adminCommand({shutdown: 1, force: true, timeoutSecs: 30});
    } catch (e) {
        // shutdown closes the connection -- exception is expected.
    }
}, target.port);

// Assert the process actually exits within the deadline. waitProgram returns the exit code once
// the mongod process is gone; if stepUpIfEligible is not interruptible, the process stays alive
// past the failpoint and this assert.soon trips.
assert.soon(
    function () {
        try {
            new Mongo(target.host);
            return false;  // Still accepting connections -> not shut down yet.
        } catch (e) {
            return true;   // Connection refused -> mongod has exited.
        }
    },
    "mongod did not shut down within " + kShutdownDeadlineMs +
        "ms despite shutdown being issued; stepUpIfEligible likely ignored the OperationContext " +
        "interrupt (SERVER-125948).",
    kShutdownDeadlineMs,
    /*interval=*/ 500,
);

const shutdownElapsed = Date.now() - shutdownStart;
jsTestLog("Target shut down in " + shutdownElapsed + "ms (deadline " + kShutdownDeadlineMs + "ms).");

// Best-effort: clear the failpoint handle even though the node is gone; configureFailPoint stashes
// the connection, and off() against a dead node is a no-op error we intentionally swallow.
try {
    fp.off();
} catch (e) {
    // expected
}

// Drain the parallel shells.
stepUpShell({checkExitSuccess: false});
shutdownShell({checkExitSuccess: false});

// Make ReplSetTest aware the node is already down so stopSet doesn't try to shutdown again.
MongoRunner.stopMongod(target, undefined, {allowedExitCode: MongoRunner.EXIT_CLEAN});

replTest.stopSet();
