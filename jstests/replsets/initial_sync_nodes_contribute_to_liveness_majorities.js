/*
 * Tests that nodes in initial sync are considered a part of the liveness majority. This test starts
 * with a two-node replica set. We then add a secondary and force it to hang in initial sync. We
 * proceed to remove all communication for the other secondary. We verify that the primary does not
 * step down and no elections were called, since the node in initial sync contributes to the
 * liveness majority. Finally, we verify that nodes in initial sync are able to vote in an election.
 * This is done by stepping down the primary, waiting for it to become a secondary, and allowing it
 * to run for primary again. Since the other secondary is disconnected, the primary must receive a
 * vote from the initial sync node to get elected again.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/rslib.js");

const name = jsTestName();
const rst = new ReplSetTest({
    name,
    nodes: [{}, {rsConfig: {priority: 0}}],
    settings: {electionTimeoutMillis: 1000, heartbeatIntervalMillis: 250},
    useBridge: true
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondaries()[0];

const initialSyncSecondary = rst.add({
    rsConfig: {priority: 0},
    setParameter: {'failpoint.initialSyncHangBeforeFinish': tojson({mode: 'alwaysOn'})},
});

rst.reInitiate();
assert.commandWorked(initialSyncSecondary.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeFinish",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

secondary.disconnect(primary);

// Verify that the primary should not step down due to not seeing a quorum. This is because the
// primary should be receiving heartbeats from the initial sync node.
assert.throws(() => checkLog.contains(
                  primary, "can't see a majority of the set, relinquishing primary", 3000));

assert.eq(primary, rst.getPrimary());

// Verify that the term is still 1, so the primary did not call any elections.
let primaryReplSetStatus = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
assert.eq(1, primaryReplSetStatus.term);

// Step down the primary and prevent it from running for elections with a high freeze timeout.
assert.commandWorked(
    primary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
waitForState(primary, ReplSetTest.State.SECONDARY);

// Unfreeze the primary so that it can run for election. It should immediately run for election as
// no other nodes are able to call for an election.
assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));

// Verify that the primary was elected.
assert.eq(primary, rst.getPrimary());

// Verify that the term has incremented due to the last election.
primaryReplSetStatus = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
assert.eq(2, primaryReplSetStatus.term);

// Verify that initial sync node voted in the election and sets the correct term in its
// electionParticipantMetrics field.
const initialSyncSecondaryReplSetStatus =
    assert.commandWorked(initialSyncSecondary.adminCommand({replSetGetStatus: 1}));
assert.eq(2, initialSyncSecondaryReplSetStatus.electionParticipantMetrics.electionTerm);

// The disconnected secondary did not vote in the last election, so its electionParticipantMetrics
// field should not be set.
const disconnectedSecondaryReplSetStatus =
    assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
assert.eq(undefined, disconnectedSecondaryReplSetStatus.electionParticipantMetrics);

// Since the primary sends a shut down command to all secondaries in `rst.stopSet()`, we reconnect
// the disconnected secondary to the primary to allow it to be shut down.
secondary.reconnect(primary);

assert.commandWorked(initialSyncSecondary.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
waitForState(initialSyncSecondary, ReplSetTest.State.SECONDARY);

rst.stopSet();
})();
