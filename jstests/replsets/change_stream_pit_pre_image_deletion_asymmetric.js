/**
 * Tests change stream point-in-time pre-images deletion replication to secondaries when primary
 * node state is not the same as of the secondary - the pre-image document to be deleted exists on
 * the primary node but does not exist on the secondary.
 *
 * @tags: [
 * requires_fcv_60,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/change_stream_util.js");  // For getPreImages().
load("jstests/libs/fail_point_util.js");
load('jstests/replsets/rslib.js');  // For getLatestOp, getFirstOplogEntry.

const oplogSizeMB = 1;
const replTest = new ReplSetTest({
    name: jsTestName(),
    nodes: [{}, {rsConfig: {priority: 0}}],
    nodeOptions: {
        setParameter: {logComponentVerbosity: tojsononeline({replication: {initialSync: 5}})},
        oplogSize: oplogSizeMB
    }
});
replTest.startSet();
replTest.initiate();
const primaryNode = replTest.getPrimary();

const collectionName = "coll";
const testDB = primaryNode.getDB(jsTestName());

// Create a collection with change stream pre- and post-images enabled.
assert.commandWorked(
    testDB.createCollection(collectionName, {changeStreamPreAndPostImages: {enabled: true}}));
const coll = testDB[collectionName];

// Insert a document for the test.
assert.commandWorked(coll.insert({_id: 1, v: 1}, {writeConcern: {w: 2}}));

// Add a new node that will perform an initial sync. Pause the initial sync process (using
// failpoint "initialSyncHangBeforeCopyingDatabases") before copying the database to perform
// document modifications to make the collection content more recent and create inconsistent
// data situation during oplog application.
const initialSyncNode = replTest.add({
    rsConfig: {priority: 0},
    setParameter: {'failpoint.initialSyncHangBeforeCopyingDatabases': tojson({mode: 'alwaysOn'})}
});

// Wait until the new node starts and pauses on the fail point.
replTest.reInitiate();
assert.commandWorked(initialSyncNode.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeCopyingDatabases",
    timesEntered: 1,
    maxTimeMS: 60000
}));

// Update the document on the primary node.
assert.commandWorked(coll.updateOne({_id: 1}, {$inc: {v: 1}}, {writeConcern: {w: 2}}));

// Resume the initial sync process.
assert.commandWorked(initialSyncNode.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCopyingDatabases", mode: "off"}));

// Wait until the initial sync process is complete and the new node becomes a fully
// functioning secondary.
replTest.waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);

// Verify that pre-images were not written during the logical initial sync. At this point the
// pre-image collections in the nodes of the replica set are out of sync.
let preImageDocuments = getPreImages(initialSyncNode);
assert.eq(preImageDocuments.length, 0, preImageDocuments);

// Force deletion of all pre-images and ensure that this replicates to all nodes.
// Roll over all current oplog entries.
const lastOplogEntryToBeRemoved = getLatestOp(primaryNode);
assert.neq(lastOplogEntryToBeRemoved, null);
const largeString = 'a'.repeat(256 * 1024);
const otherColl = primaryNode.getDB(jsTestName())["otherCollection"];

// Checks if the oplog has been rolled over from the timestamp of
// 'lastOplogEntryToBeRemoved', ie. the timestamp of the first entry in the oplog is greater
// than 'lastOplogEntryToBeRemoved'.
function oplogIsRolledOver() {
    return timestampCmp(lastOplogEntryToBeRemoved.ts,
                        getFirstOplogEntry(primaryNode, {readConcern: "majority"}).ts) <= 0;
}

while (!oplogIsRolledOver()) {
    // Insert a large document with a write concern that ensures that before proceeding the
    // operation gets replicated to all 3 nodes in the replica set, since, otherwise, the node that
    // is being initial synced may not be able to catchup due to a small size of the oplog.
    assert.commandWorked(otherColl.insert({long_str: largeString}, {writeConcern: {w: 3}}));
}

// Wait until 'PeriodicChangeStreamExpiredPreImagesRemover' job deletes the expired pre-images
// (all).
assert.soon(() => {
    const preImages = getPreImages(primaryNode);
    return preImages.length == 0;
});

// Verify that all nodes get in sync and do not crash.
replTest.awaitReplication();
replTest.stopSet();
})();
