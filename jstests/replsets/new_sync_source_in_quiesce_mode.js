/*
 * Test that fetching oplog from a new sync source that is in quiesce mode fails to establish a
 * connection, causing the server to reenter sync source selection.
 *
 * @tags: [requires_fcv_46]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/write_concern_util.js");

const rst = new ReplSetTest({
    name: "new_sync_source_in_quiesce_mode",
    nodes: 3,
    nodeOptions: {setParameter: "shutdownTimeoutMillisForSignaledShutdown=5000"}
});
rst.startSet();
const syncSource = rst.nodes[1];
const syncingNode = rst.nodes[2];

// Make sure the syncSource syncs only from the new primary. This is so that we prevent
// syncingNode from blacklisting syncSource because it isn't syncing from anyone.
assert.commandWorked(syncSource.adminCommand({
    configureFailPoint: "forceSyncSourceCandidate",
    mode: "alwaysOn",
    data: {hostAndPort: rst.nodes[0].host}
}));
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();

// Stop replication on the syncingNode so that the primary and syncSource will both
// definitely be ahead of it.
stopServerReplication(syncingNode);

jsTestLog("Ensure syncSource is ahead of syncingNode.");
// Write some data on the primary, which will only be replicated to the syncSource.
assert.commandWorked(primary.getDB("test").c.insert({a: 1}), {writeConcern: {w: 2}});

jsTestLog("Transition syncSource to quiesce mode.");
let quiesceModeFailPoint = configureFailPoint(syncSource, "hangDuringQuiesceMode");
rst.stop(syncSource, null /*signal*/, null /*opts*/, {forRestart: true, waitpid: false});
quiesceModeFailPoint.wait();

jsTestLog("Ensure syncingNode tries to sync from syncSource.");
// Use the replSetSyncFrom command to try and connect to the syncSource in quiesce mode.
assert.commandWorked(syncingNode.adminCommand({replSetSyncFrom: syncSource.name}));
restartServerReplication(syncingNode);
// We will have blacklisted syncSource since it is shutting down, so we should re-enter
// sync source selection and eventually choose the primary as our sync source.
rst.awaitSyncSource(syncingNode, primary);

jsTestLog("Restart syncSource.");
quiesceModeFailPoint.off();
rst.restart(syncSource);
rst.awaitSecondaryNodes();

jsTestLog("Finish test.");
rst.stopSet();
})();