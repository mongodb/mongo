/**
 * Verifies that a primary which grants its vote while concurrently stepping down still persists
 * that vote, letting the candidate win in a single election round (the step-up optimization from
 * SERVER-34682). The vote is recorded in the granting node's TopologyCoordinator, but if the write
 * that persists it never completes, the vote is "spoiled" (log 21428) and the candidate must retry
 * at a higher term.
 *
 * Reproducing the spoiled vote requires a specific interleaving where the old primary must learn the
 * candidate's new term via heartbeat before the vote request arrives (a vote request carrying the
 * new term itself blocks on the stepdown inside updateTerm and cannot race it). The test forces
 * that ordering with three failpoints:
 * - "hangInWritingLastVoteForDryRun" on the candidate pauses it between incrementing its term and
 *   requesting votes, giving the old primary time to learn the new term via heartbeat first.
 * - "blockHeartbeatStepdown" on the old primary pauses its stepdown before the kill-ops phase, so
 *   the vote write can be parked before the sweep runs.
 * - "hangAfterAcquiringLastVoteCollection" on the old primary pauses the lastVote write while it
 *   holds the global IX lock which is what makes it a target of the kill-ops phase.
 *
 * TODO(SERVER-91733): Remove the need for the failpoints once intent based kill-ops is used
 * exclusively.
 *
 * @tags: [requires_fcv_90]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Periodic noops could advance the old primary's lastWritten past the paused candidate's and
// deny the vote.
const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: {writePeriodicNoops: false}}});
rst.startSet();
rst.initiate();

const oldPrimary = rst.getPrimary();
const candidate = rst.getSecondary();
rst.awaitReplication();

const firstTerm = assert.commandWorked(oldPrimary.adminCommand({replSetGetStatus: 1})).term;

const candidateFp = configureFailPoint(candidate, "hangInWritingLastVoteForDryRun");
const stepdownFp = configureFailPoint(oldPrimary, "blockHeartbeatStepdown");
const storeFp = configureFailPoint(oldPrimary, "hangAfterAcquiringLastVoteCollection");

// Raw replSetStepUp because ReplSetTest.stepUp retries on a lost election, which would mask
// the lost round.
const awaitStepUp = startParallelShell(function () {
    assert.commandWorked(db.adminCommand({replSetStepUp: 1}));
}, candidate.port);

jsTestLog("Waiting for the candidate to win the dry run and increment its term");
candidateFp.wait();

jsTestLog(
    "Waiting for the old primary to learn the new term via heartbeat and begin stepping down",
);
stepdownFp.wait();

jsTestLog("Releasing the candidate to request votes");
candidateFp.off();

jsTestLog("Waiting for the old primary to grant its vote and park the lastVote write");
storeFp.wait();

// Identify the connection serving the parked vote request. The stepdown sweep logs 8562701
// naming this connection when it interrupts the op, which works as the barrier in both the
// fixed and unfixed cases: the sweep logs it whether or not the op honors the interrupt, so it
// does not depend on the spoiled-vote symptom we are trying to suppress.
const parkedOps = oldPrimary
    .getDB("admin")
    .aggregate([
        {$currentOp: {allUsers: true, idleConnections: true}},
        {$match: {"command.replSetRequestVotes": {$exists: true}}},
    ])
    .toArray();
assert.eq(1, parkedOps.length, "expected exactly one parked vote request", {parkedOps});
const voteConn = parkedOps[0].desc;

jsTestLog("Releasing the stepdown so its kill-ops phase targets the parked lastVote write");
stepdownFp.off();

assert.soon(
    () => checkLog.checkContainsOnceJson(oldPrimary, 8562701, {name: voteConn}),
    "expected the stepdown to interrupt the parked vote request",
);

jsTestLog("Releasing the lastVote write");
storeFp.off();

// A spoiled vote makes the candidate lose this single-attempt election, failing replSetStepUp.
awaitStepUp();

rst.awaitNodesAgreeOnPrimary();
assert.eq(candidate, rst.getPrimary());

const newTerm = assert.commandWorked(candidate.adminCommand({replSetGetStatus: 1})).term;
assert.eq(firstTerm + 1, newTerm, "expected the election to succeed in a single round");

assert(
    !checkLog.checkContainsOnceJson(oldPrimary, 21428),
    "the old primary failed to persist the vote it granted",
);
oldPrimary.setSecondaryOk();
const lastVote = oldPrimary.getDB("local").getCollection("replset.election").findOne();
assert.eq(firstTerm + 1, lastVote.term, "granted vote was not persisted", {lastVote});
assert.eq(rst.getNodeId(candidate), lastVote.candidateIndex, "vote persisted for wrong candidate", {
    lastVote,
});

rst.stopSet();
