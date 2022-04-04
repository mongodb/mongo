// Tests that expired pre-images (pre-image timestamp older than oldest oplog entry timestamp) are
// removed from the pre-images collection via the 'PeriodicChangeStreamExpiredPreImagesRemover'
// periodic job.
// @tags: [
//  requires_fcv_60,
//  featureFlagChangeStreamPreAndPostImages,
//  assumes_against_mongod_not_mongos,
//  change_stream_does_not_expect_txns,
//  requires_replication,
//  requires_majority_read_concern,
// ]
(function() {
"use strict";

load('jstests/replsets/rslib.js');                 // For getLatestOp, getFirstOplogEntry.
load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.

const docA = {
    _id: 12345,
    version: 1,
};
const docB = {
    _id: 12345,
    version: 2,
};
const docC = {
    _id: 12345,
    version: 3,
};
const preImagesCollectionDatabase = "config";
const preImagesCollectionName = "system.preimages";
const oplogSizeMB = 1;

// Set up the replica set with two nodes and two collections with 'changeStreamPreAndPostImages'
// enabled and run expired pre-image removal job every second.
const rst = new ReplSetTest({nodes: 2, oplogSize: oplogSizeMB});
rst.startSet({setParameter: {expiredChangeStreamPreImageRemovalJobSleepSecs: 1}});
rst.initiate();
const largeStr = 'abcdefghi'.repeat(4 * 1024);
const primaryNode = rst.getPrimary();
const testDB = primaryNode.getDB(jsTestName());
const localDB = primaryNode.getDB("local");

// Returns documents from the pre-images collection from 'node'.
function getPreImages(node) {
    return node.getDB(preImagesCollectionDatabase)[preImagesCollectionName].find().toArray();
}

// Checks if the oplog has been rolled over from the timestamp of
// 'lastOplogEntryTsToBeRemoved', ie. the timestamp of the first entry in the oplog is greater
// than the 'lastOplogEntryTsToBeRemoved' on each node of the replica set.
function oplogIsRolledOver(lastOplogEntryTsToBeRemoved) {
    return [primaryNode, rst.getSecondary()].every(
        (node) => timestampCmp(lastOplogEntryTsToBeRemoved,
                               getFirstOplogEntry(node, {readConcern: "majority"}).ts) <= 0);
}

// Tests that the pre-image removal job deletes only the expired pre-images by performing four
// updates leading to four pre-images being recorded, then the oplog is rolled over, removing the
// oplog entries of the previously recorded pre-images. Afterwards two updates are performed and
// therefore two new pre-images are recorded. The pre-images removal job must remove only the first
// four recorded pre-images.
// 'batchedDelete' determines whether pre-images will be removed in batches or document-by-document.
function testPreImageRemovalJob(batchedDelete) {
    // Roll over the oplog, leading to 'PeriodicChangeStreamExpiredPreImagesRemover' periodic job
    // deleting all pre-images.
    let lastOplogEntryToBeRemoved = getLatestOp(primaryNode);
    while (!oplogIsRolledOver(lastOplogEntryToBeRemoved.ts)) {
        assert.commandWorked(
            testDB.tmp.insert({long_str: largeStr}, {writeConcern: {w: "majority"}}));
    }
    assert.soon(() => getPreImages(primaryNode).length == 0);

    // Set the 'batchedExpiredChangeStreamPreImageRemoval'.
    assert.commandWorked(primaryNode.adminCommand(
        {setParameter: 1, batchedExpiredChangeStreamPreImageRemoval: batchedDelete}));

    // Drop and recreate the collections with pre-images recording.
    const collA = assertDropAndRecreateCollection(
        testDB, "collA", {changeStreamPreAndPostImages: {enabled: true}});
    const collB = assertDropAndRecreateCollection(
        testDB, "collB", {changeStreamPreAndPostImages: {enabled: true}});

    // Perform insert and update operations.
    for (const coll of [collA, collB]) {
        assert.commandWorked(coll.insert(docA, {writeConcern: {w: "majority"}}));
        assert.commandWorked(coll.update(docA, {$inc: {version: 1}}));
        assert.commandWorked(coll.update(docB, {$inc: {version: 1}}));
    }

    // Pre-images collection should contain four pre-images.
    let preImages = getPreImages(primaryNode);
    const preImagesToExpire = 4;
    assert.eq(preImages.length, preImagesToExpire, preImages);

    // Roll over all current oplog entries.
    lastOplogEntryToBeRemoved = getLatestOp(primaryNode);
    assert.neq(lastOplogEntryToBeRemoved, null);

    // Checks if the oplog has been rolled over from the timestamp of
    // 'lastOplogEntryToBeRemoved', ie. the timestamp of the first entry in the oplog is greater
    // than the timestamp of the 'lastOplogEntryToBeRemoved' on each node of the replica set.
    while (!oplogIsRolledOver(lastOplogEntryToBeRemoved.ts)) {
        assert.commandWorked(collA.insert({long_str: largeStr}, {writeConcern: {w: "majority"}}));
    }

    // Perform update operations that insert new pre-images that are not expired yet.
    for (const coll of [collA, collB]) {
        assert.commandWorked(coll.update(docC, {$inc: {version: 1}}));
    }

    // Wait until 'PeriodicChangeStreamExpiredPreImagesRemover' periodic job will delete the expired
    // pre-images.
    assert.soon(() => {
        // Only two pre-images should still be there, as their timestamp is greater than the oldest
        // oplog entry timestamp.
        preImages = getPreImages(primaryNode);
        const onlyTwoPreImagesLeft = preImages.length == 2;
        const allPreImagesHaveBiggerTimestamp = preImages.every(
            preImage => timestampCmp(preImage._id.ts, lastOplogEntryToBeRemoved.ts) == 1);
        return onlyTwoPreImagesLeft && allPreImagesHaveBiggerTimestamp;
    });

    // Because the pre-images collection is implicitly replicated, validate that writes do not
    // generate oplog entries, with the exception of deletions.
    const preimagesNs = 'config.system.preimages';
    if (batchedDelete) {
        const serverStatusBatches = testDB.serverStatus()['batchedDeletes']['batches'];
        const serverStatusDocs = testDB.serverStatus()['batchedDeletes']['docs'];
        assert.eq(serverStatusBatches, 2);
        assert.eq(serverStatusDocs, preImagesToExpire);

        // Multi-deletes are batched base on time before performing the deletion, therefore the
        // deleted pre-images can span through multiple applyOps oplog entries.
        assert.gte(preImagesToExpire,
                   localDB.oplog.rs
                       .find({ns: 'admin.$cmd', 'o.applyOps.op': 'd', 'o.applyOps.ns': preimagesNs})
                       .itcount());
    } else {
        assert.eq(preImagesToExpire, localDB.oplog.rs.find({op: 'd', ns: preimagesNs}).itcount());
    }
    assert.eq(0, localDB.oplog.rs.find({op: {'$ne': 'd'}, ns: preimagesNs}).itcount());

    // Verify that pre-images collection content on the primary node is the same as on the
    // secondary.
    rst.awaitReplication();
    assert(bsonWoCompare(getPreImages(primaryNode), getPreImages(rst.getSecondary())) === 0);
}

for (const batchedDelete of [false, true]) {
    testPreImageRemovalJob(batchedDelete);
}

// Increase oplog size on each node to prevent oplog entries from being deleted which removes a
// risk of replica set consistency check failure during tear down of the replica set.
const largeOplogSizeMB = 1000;
rst.nodes.forEach((node) => {
    assert.commandWorked(node.adminCommand({replSetResizeOplog: 1, size: largeOplogSizeMB}));
});

rst.stopSet();
}());
