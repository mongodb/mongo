// Tests that expired pre-images (pre-image timestamp older than oldest oplog entry timestamp) are
// removed from the pre-images collection via the 'PeriodicChangeStreamExpiredPreImagesRemover'
// periodic job.
// @tags: [
//  requires_fcv_51,
//  featureFlagChangeStreamPreAndPostImages,
//  # Clustered index support is required for change stream pre-images collection.
//  featureFlagClusteredIndexes,
//  assumes_against_mongod_not_mongos,
//  change_stream_does_not_expect_txns,
//  multiversion_incompatible,
// ]
(function() {
"use strict";

load('jstests/replsets/rslib.js');                 // For getLatestOp, getFirstOplogEntry.
load("jstests/libs/collection_drop_recreate.js");  // For assertDropCollection,
                                                   // assertDropAndRecreateCollection.

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

for (const isChangeStreamExpiredPreImageRemovalJobReplicating of [false, true]) {
    jsTest.log(
        "Testing with the parameter 'isChangeStreamExpiredPreImageRemovalJobReplicating' set to " +
        isChangeStreamExpiredPreImageRemovalJobReplicating);
    const rst = new ReplSetTest({nodes: 2, oplogSize: oplogSizeMB});

    // Run expired pre-image removal job every second.
    rst.startSet({
        setParameter: {
            expiredChangeStreamPreImageRemovalJobSleepSecs: 1,
            isChangeStreamExpiredPreImageRemovalJobReplicating:
                isChangeStreamExpiredPreImageRemovalJobReplicating
        }
    });
    rst.initiate();

    const primaryNode = rst.getPrimary();
    const testDB = primaryNode.getDB(jsTestName());
    assertDropCollection(primaryNode.getDB(preImagesCollectionDatabase), preImagesCollectionName);

    const collA = assertDropAndRecreateCollection(
        testDB, "collA", {changeStreamPreAndPostImages: {enabled: true}});
    const collB = assertDropAndRecreateCollection(
        testDB, "collB", {changeStreamPreAndPostImages: {enabled: true}});

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
    assert.eq(preImages.length, 4, preImages);

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

    // Verify that pre-images collection content on the primary node is the same as on the
    // secondary. Do that asynchronously since, when pre-image deletes are not replicated, ensuring
    // that writes have been replicated is not enough to achieve a consistent state.
    assert.soon(() => {
        return bsonWoCompare(getPreImages(primaryNode), getPreImages(rst.getSecondary())) === 0;
    });

    // Increase oplog size on each node to prevent oplog entries from being deleted which removes a
    // risk of replica set consistency check failure during tear down of the replica set.
    const largeOplogSizeMB = 1000;
    rst.nodes.forEach((node) => {
        assert.commandWorked(node.adminCommand({replSetResizeOplog: 1, size: largeOplogSizeMB}));
    });

    rst.stopSet();
}
}());
