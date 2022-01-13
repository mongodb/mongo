// Tests that expired pre-images (pre-image timestamp older than oldest oplog entry timestamp) are
// removed from the pre-images collection via the 'PeriodicChangeStreamExpiredPreImagesRemover'
// periodic job.
// @tags: [
//  requires_fcv_52,
//  featureFlagChangeStreamPreAndPostImages,
//  assumes_against_mongod_not_mongos,
//  change_stream_does_not_expect_txns,
// ]
(function() {
"use strict";

load('jstests/replsets/rslib.js');                 // For getLatestOp, getFirstOplogEntry.
load("jstests/libs/collection_drop_recreate.js");  // For assertCreateCollection.

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

// Returns documents from the pre-images collection from 'node'.
function getPreImages(node) {
    return node.getDB(preImagesCollectionDatabase)[preImagesCollectionName].find().toArray();
}

// Set up the replica set with two nodes and two collections with 'changeStreamPreAndPostImages'
// enabled and run expired pre-image removal job every second.
const rst = new ReplSetTest({nodes: 2, oplogSize: oplogSizeMB});
rst.startSet({setParameter: {expiredChangeStreamPreImageRemovalJobSleepSecs: 1}});
rst.initiate();
const primaryNode = rst.getPrimary();
const testDB = primaryNode.getDB(jsTestName());
const localDB = primaryNode.getDB("local");
const collA =
    assertCreateCollection(testDB, "collA", {changeStreamPreAndPostImages: {enabled: true}});
const collB =
    assertCreateCollection(testDB, "collB", {changeStreamPreAndPostImages: {enabled: true}});

// Pre-images collection must be empty.
let preImages = getPreImages(primaryNode);
assert.eq(preImages.length, 0, preImages);

// Perform insert and update operations.
for (const coll of [collA, collB]) {
    assert.commandWorked(coll.insert(docA, {writeConcern: {w: "majority"}}));
    assert.commandWorked(coll.update(docA, {$inc: {version: 1}}));
    assert.commandWorked(coll.update(docB, {$inc: {version: 1}}));
}

// Pre-images collection should contain four pre-images.
preImages = getPreImages(primaryNode);
const preImagesToExpire = 4;
assert.eq(preImages.length, preImagesToExpire, preImages);

// Roll over all current oplog entries.
const lastOplogEntryToBeRemoved = getLatestOp(primaryNode);
assert.neq(lastOplogEntryToBeRemoved, null);
const largeStr = 'abcdefghi'.repeat(4 * 1024);

// Checks if the oplog has been rolled over from the timestamp of
// 'lastOplogEntryToBeRemoved', ie. the timestamp of the first entry in the oplog is greater
// than the timestamp of the 'lastOplogEntryToBeRemoved' on each node of the replica set.
function oplogIsRolledOver() {
    return [primaryNode, rst.getSecondary()].every(
        (node) => timestampCmp(lastOplogEntryToBeRemoved.ts,
                               getFirstOplogEntry(node, {readConcern: "majority"}).ts) <= 0);
}

while (!oplogIsRolledOver()) {
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

// Because the pre-images collection is implicitly replicated, validate that writes do not generate
// oplog entries, with the exception of deletions.
const preimagesNs = 'config.system.preimages';
assert.eq(preImagesToExpire, localDB.oplog.rs.find({op: 'd', ns: preimagesNs}).itcount());
assert.eq(0, localDB.oplog.rs.find({op: {'$ne': 'd'}, ns: preimagesNs}).itcount());

// Verify that pre-images collection content on the primary node is the same as on the
// secondary.
rst.awaitReplication();
assert(bsonWoCompare(getPreImages(primaryNode), getPreImages(rst.getSecondary())) === 0);

// Increase oplog size on each node to prevent oplog entries from being deleted which removes a
// risk of replica set consistency check failure during tear down of the replica set.
const largeOplogSizeMB = 1000;
rst.nodes.forEach((node) => {
    assert.commandWorked(node.adminCommand({replSetResizeOplog: 1, size: largeOplogSizeMB}));
});

rst.stopSet();
}());
