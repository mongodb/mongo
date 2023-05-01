/**
 * Tests that nodes in initial sync update their commit point and gossip their commit point to
 * other nodes. This is done starting with a 3 node replica set with one non-voting secondary. We
 * disconnect the non-voting secondary from the other nodes and then add a new node to the replica
 * set. Thus, the non-voting secondary can only communicate with the initial syncing node. We then
 * hang the initial syncing node at various stages, perform multiple majority writes to advance the
 * commit point, and verify that the commit point on the initial syncing node is updated. Finally,
 * we ensure that the disconnected secondary is able to update its commit point from the initial
 * syncing node via heartbeats.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/rslib.js");

const name = jsTestName();
const rst = new ReplSetTest({
    name,
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0, votes: 0}}],
    useBridge: true
});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();
const primaryDb = primary.getDB("test");
const secondary = rst.getSecondaries()[0];
const nonVotingSecondary = rst.getSecondaries()[1];

// Insert initial data to ensure that the repl set is initialized correctly.
assert.commandWorked(primaryDb.test.insert({a: 1}));
rst.awaitReplication();

/*
 * Fetches the 'lastCommittedOpTime' field of the given node.
 */
function getLastCommittedOpTime(conn) {
    const replSetStatus = assert.commandWorked(conn.adminCommand({replSetGetStatus: 1}));
    return replSetStatus.optimes.lastCommittedOpTime;
}

const firstCommitPoint = getLastCommittedOpTime(primary);
jsTestLog(`First commit point: ${tojson(firstCommitPoint)}`);

// Disconnect the non-voting secondary from the other nodes so that it won't update its commit point
// from the other nodes' heartbeats.
nonVotingSecondary.disconnect(secondary);
nonVotingSecondary.disconnect(primary);

jsTest.log("Adding a new node to the replica set");
const initialSyncNode = rst.add({
    setParameter: {
        // Make sure our initial sync node does not sync from the node with votes 0.
        'failpoint.forceSyncSourceCandidate':
            tojson({mode: 'alwaysOn', data: {"hostAndPort": primary.host}}),
    }
});

const hangAfterGettingBeginFetchingTimestamp =
    configureFailPoint(initialSyncNode, "initialSyncHangAfterGettingBeginFetchingTimestamp");
const hangBeforeCompletingOplogFetching =
    configureFailPoint(initialSyncNode, "initialSyncHangBeforeCompletingOplogFetching");
const hangBeforeFinish = configureFailPoint(initialSyncNode, "initialSyncHangBeforeFinish");

jsTestLog("Waiting for initial sync node to reach initial sync state");
rst.reInitiate();
rst.waitForState(initialSyncNode, ReplSetTest.State.STARTUP_2);

// Hang the initial sync node after it sets 'beginFetchingTimestamp' to ensure that the node will
// not set 'stopTimestamp' until after we perform the next write.
hangAfterGettingBeginFetchingTimestamp.wait();

// Do a write to be applied by the initial sync node. This is necessary because we cannot update the
// commit point if the term of `lastAppliedOpTime` is not equal to the current term.
assert.commandWorked(primaryDb.test.insert({b: 2}));
// Wait for knowledge of the last commit point to advance to the last write on the primary and
// secondary.
rst.awaitLastOpCommitted(undefined, [primary, secondary]);

const secondCommitPointPrimary = getLastCommittedOpTime(primary);
const secondCommitPointSecondary = getLastCommittedOpTime(secondary);
jsTestLog(`Second commit point: ${tojson(secondCommitPointPrimary)}`);

// Verify that the commit point has advanced on the primary and secondary.
assert.eq(1, rs.compareOpTimes(secondCommitPointPrimary, firstCommitPoint));
assert.eq(1, rs.compareOpTimes(secondCommitPointSecondary, firstCommitPoint));

// Verify that the commit point has *NOT* advanced on the non-voting secondary.
const commitPointNonVotingSecondary = getLastCommittedOpTime(nonVotingSecondary);
assert.eq(rs.compareOpTimes(commitPointNonVotingSecondary, secondCommitPointPrimary),
          -1,
          `commit point on the non-voting secondary should not have been advanced: ${
              tojson(commitPointNonVotingSecondary)}`);

// Allow the node to proceed to the oplog applying phase of initial sync and ensure that the oplog
// fetcher thread is still running.
hangAfterGettingBeginFetchingTimestamp.off();
hangBeforeCompletingOplogFetching.wait();

// The initial sync node will be able to update its commit point after fetching this write, since it
// set its `lastAppliedOpTime` from the previous write.
assert.commandWorked(primaryDb.test.insert({c: 3}));
// Wait for knowledge of the last commit point to advance to the last write on the primary and
// secondary.
rst.awaitLastOpCommitted(undefined, [primary, secondary]);

const thirdCommitPointPrimary = getLastCommittedOpTime(primary);
const thirdCommitPointSecondary = getLastCommittedOpTime(secondary);
jsTestLog(`Third commit point: ${tojson(thirdCommitPointPrimary)}`);

// Verify that the commit point has advanced on the primary and secondary.
assert.eq(1, rs.compareOpTimes(thirdCommitPointPrimary, secondCommitPointPrimary));
assert.eq(1, rs.compareOpTimes(thirdCommitPointSecondary, secondCommitPointSecondary));

// Allow the initial sync node to complete oplog fetching but hang it before it completes initial
// sync.
hangBeforeCompletingOplogFetching.off();
hangBeforeFinish.wait();

// Verify that the initial sync node receives the commit point from the primary, either via oplog
// fetching or by a heartbeat. This will usually happen via oplog fetching but in some cases it is
// possible that the OplogFetcher shuts down before this ever happens. See SERVER-76695 for details.
// We only assert that it is greater than or equal to the second commit point because it is possible
// for the commit point to not yet be advanced by the primary when we fetch the oplog entry.
assert.soon(() => {
    const commitPointInitialSyncNode = getLastCommittedOpTime(initialSyncNode);
    // compareOpTimes will throw an error if given an invalid opTime, and if the
    // node has not yet advanced its opTime it will still have the default one,
    // which is invalid.
    if (!globalThis.rs.isValidOpTime(commitPointInitialSyncNode)) {
        return false;
    }
    return rs.compareOpTimes(commitPointInitialSyncNode, secondCommitPointPrimary) >= 0;
}, `commit point on initial sync node should be at least as up-to-date as the second commit point`);

// Verify that the non-voting secondary has received the updated commit point via heartbeats from
// the initial sync node.
assert.soon(
    () => rs.compareOpTimes(getLastCommittedOpTime(nonVotingSecondary),
                            getLastCommittedOpTime(initialSyncNode)) >= 0,
    "The nonVotingSecondary was unable to update its commit point from the initial sync node");

// Since the primary sends a shut down command to all secondaries in `rst.stopSet()`, we reconnect
// the disconnected secondary to the primary to allow it to be shut down.
nonVotingSecondary.reconnect(primary);

hangBeforeFinish.off();
waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);

rst.stopSet();
})();
