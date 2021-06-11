/**
 * Tests that reading from an existing sync source continues uninterrupted when the sync source
 * enters quiesce mode.
 *
 * @tags: [
 *   live_record_incompatible,
 *   requires_fcv_47,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

// Set the oplog fetcher batch size to 1, in order to test fetching multiple batches while the sync
// source is in quiesce mode.
const rst = new ReplSetTest(
    {nodes: 3, useBridge: true, nodeOptions: {setParameter: "bgSyncOplogFetcherBatchSize=1"}});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();
const testDB = primary.getDB("testDB");
const testCollName = "testColl";
assert.eq(primary, rst.nodes[0]);

const syncSource = rst.nodes[1];
const syncingNode = rst.nodes[2];

jsTestLog("Ensure syncingNode is syncing from syncSource.");
syncingNode.disconnect(primary);
assert.commandWorked(
    testDB.runCommand({insert: testCollName, documents: [{a: 1}], writeConcern: {w: 3}}));

jsTestLog("Ensure syncingNode is behind syncSource.");
let hangSyncingNodeFailPoint =
    configureFailPoint(syncingNode, "hangBeforeProcessingSuccessfulBatch");
assert.commandWorked(testDB.runCommand(
    {insert: testCollName, documents: [{b: 2}, {c: 3}, {d: 4}], writeConcern: {w: 2}}));
hangSyncingNodeFailPoint.wait();

jsTestLog("Transition syncSource to quiesce mode.");
let quiesceModeFailPoint = configureFailPoint(syncSource, "hangDuringQuiesceMode");
// We must skip validation due to the failpoint that hangs awaitData queries.
rst.stop(syncSource, null /*signal*/, {skipValidation: true}, {forRestart: true, waitpid: false});
quiesceModeFailPoint.wait();

jsTestLog("Check that syncing continues uninterrupted.");
hangSyncingNodeFailPoint.off();
rst.awaitReplication();

jsTestLog("Finish test.");
syncingNode.reconnect(primary);
quiesceModeFailPoint.off();
rst.restart(syncSource);
rst.stopSet();
})();
