/**
 * Regression test for the standby-loop / becomePrimary() race in the disaggregated
 * replication coordinator. During step-up, stepUpIfEligible() calls
 * cancelStateMachine() + stateMachineJoin() *before* acquiring the RSTL, but then
 * spends measurable time acquiring the RSTL and other locks before reaching
 * accept(kStepUp) / becomePrimary(). A queued accept(kDisconnected) callback can
 * fire on a different thread in that window, calling reconnectAsSecondary() and
 * starting a fresh standby loop. When becomePrimary() subsequently tears down or
 * rebinds the underlying gRPC stream, the racing standby loop's first
 * ClientReader::Read() trips a fatal CallOpRecvInitialMetadata assertion inside
 * gRPC.
 *
 * The TLA+ companion spec (src/mongo/tla_plus/Replication/StandbyStepUpRace)
 * proves NoConcurrentStandbyAndBecomePrimary under the fix and exhibits a
 * counter-example trace ending in grpcFatal = TRUE under the bug configuration.
 *
 * This test exercises the same window against a real mongod: it forces a step-up
 * after slowing the standby exit path with a failpoint so that a Disconnected
 * dispatch has the widest possible window to race becomePrimary(). The test then
 * asserts that:
 *   1) replSetStepUp succeeds and the target node ends up as primary,
 *   2) no fatal gRPC assertion was logged on any node, and
 *   3) no tassert / invariant failure was logged on any node.
 *
 * Tags: replica_sets only; the disagg failpoint is a non-fatal probe (the test
 * still asserts the no-fatal contract on builds where the failpoint is absent,
 * which is the safety guarantee SERVER-126278 fixes).
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kStandbySlowdownFailpoint = "hangBeforeStandbyExitDuringStepUp";

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: 3,
    settings: {chainingAllowed: false, catchUpTimeoutMillis: 0},
});

rst.startSet();
rst.initiate();

const originalPrimary = rst.getPrimary();
const newPrimary = rst.getSecondaries()[0];
const witness = rst.getSecondaries()[1];

assert.commandWorked(
    originalPrimary.getDB("test").runCommand({insert: "c", documents: [{_id: 0}], writeConcern: {w: "majority"}}),
);
rst.awaitReplication();

// Try to install the slowdown failpoint on the prospective new primary so the
// standby-loop exit path stalls during the step-up handler. If the build is
// compiled without the disagg replication coordinator, this failpoint will not
// exist; in that case we still proceed with the no-fatal assertion, which is
// the load-bearing guarantee of the fix.
let standbyFp = null;
let standbyFpInstalled = false;
try {
    standbyFp = configureFailPoint(newPrimary, kStandbySlowdownFailpoint, {}, "alwaysOn");
    standbyFpInstalled = true;
    jsTestLog(`Installed ${kStandbySlowdownFailpoint} on ${newPrimary.host} to widen the race window.`);
} catch (e) {
    // Failpoint not registered in this build (disagg replication coordinator
    // not compiled in). We continue: the post-step-up log assertions are still
    // valid and would catch the race if it occurred.
    jsTestLog(
        `Failpoint ${kStandbySlowdownFailpoint} not available on ${newPrimary.host} ` +
            `(likely not a disagg build). Proceeding without slowdown; log assertions still active. ` +
            `Error: ${tojson(e)}`,
    );
}

// Trigger a step-up. We retry because the original primary may step down and
// briefly reject the request, mirroring the canonical jstests/replsets/stepup.js
// pattern.
let numStepUpCmds = 0;
assert.soonNoExcept(function () {
    numStepUpCmds++;
    return newPrimary.adminCommand({replSetStepUp: 1}).ok;
}, "replSetStepUp never succeeded against new primary");

// If we installed the failpoint, let the standby exit unblock now -- the
// race window has been opened wide enough that any Disconnected callback
// queued during step-up has had a chance to interleave.
if (standbyFpInstalled) {
    try {
        standbyFp.off();
    } catch (e) {
        // If the node was tassert'd, turning the failpoint off may fail. Let
        // the log assertions below report the actual problem.
        jsTestLog(`Failed to disable failpoint (node may already be down): ${tojson(e)}`);
    }
}

// Confirm step-up succeeded.
rst.awaitNodesAgreeOnPrimary(rst.timeoutMS, rst.nodes, newPrimary);
assert.eq(newPrimary, rst.getPrimary(), "new primary did not win the step-up");

// Drive a tiny write through the new primary to prove it can act as primary.
assert.commandWorked(
    newPrimary.getDB("test").runCommand({insert: "c", documents: [{_id: 1}], writeConcern: {w: "majority"}}),
);
rst.awaitReplication();

// Inspect captured stdout/stderr for fatal gRPC assertions and tasserts. The
// stack trace in SERVER-126278 contains both an absl log_internal::LogMessage
// FailWithoutStackTrace frame and the grpc_core::FilterStackCall::ExternalUnref
// teardown -- either signature is sufficient evidence of the race.
const programOutput = rawMongoProgramOutput(".*");
const fatalGrpcSignatures = [
    "FilterStackCall::ExternalUnref",
    "CallOpRecvInitialMetadata",
    "log_internal::LogMessage::FailWithoutStackTrace",
];
for (const sig of fatalGrpcSignatures) {
    assert.eq(
        false,
        programOutput.includes(sig),
        `Found fatal gRPC signature '${sig}' in mongod output; ` +
            `SERVER-126278 race may have triggered. Step-up retries=${numStepUpCmds}, ` +
            `failpoint installed=${standbyFpInstalled}.`,
    );
}

// Any tassert / invariant / fatal-assertion log on any node fails the test.
// We scan rawMongoProgramOutput rather than each node's log file so we catch
// crashes that happened before the test could even open a connection.
const fatalTassertSignatures = ["Fatal assertion", "Invariant failure", "tassert"];
for (const sig of fatalTassertSignatures) {
    // tassert is a very common substring in healthy logs (e.g. "tasserted")
    // only when followed by a numeric ID; tighten the match for that case.
    const pattern = sig === "tassert" ? /tassert \d+/ : new RegExp(sig);
    assert.eq(
        false,
        pattern.test(programOutput),
        `Found '${sig}' in mongod output; step-up path tripped a fatal. ` +
            `Step-up retries=${numStepUpCmds}, failpoint installed=${standbyFpInstalled}.`,
    );
}

// Make sure the witness third node is still healthy and agrees on the primary.
const witnessStatus = assert.commandWorked(witness.adminCommand({replSetGetStatus: 1}));
const primaryMembers = witnessStatus.members.filter((m) => m.stateStr === "PRIMARY");
assert.eq(1, primaryMembers.length, `Witness sees ${primaryMembers.length} primaries; expected exactly 1.`);
assert.eq(
    newPrimary.host,
    primaryMembers[0].name,
    `Witness primary is ${primaryMembers[0].name}; expected ${newPrimary.host}.`,
);

rst.stopSet();
