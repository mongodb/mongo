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

const oplogSizeMB = 1;
// TODO SERVER-60238: run this test on multi node replica set, when pre-image replication to
// secondaries is implemented.
const rst = new ReplSetTest({nodes: 1, oplogSize: oplogSizeMB});

// Run expired pre-image removal job every second.
rst.startSet({setParameter: {expiredChangeStreamPreImageRemovalJobSleepSecs: 1}});
rst.initiate();

const primaryNode = rst.getPrimary();
const testDB = primaryNode.getDB(jsTestName());
const configDB = primaryNode.getDB("config");
const preImagesCollName = "system.preimages";
assertDropCollection(configDB, preImagesCollName);

const collA = assertDropAndRecreateCollection(
    testDB, "collA", {changeStreamPreAndPostImages: {enabled: true}});
const collB = assertDropAndRecreateCollection(
    testDB, "collB", {changeStreamPreAndPostImages: {enabled: true}});
const preImagesColl = configDB.getCollection(preImagesCollName);

// Pre-images collection must be empty.
let preImages = preImagesColl.find().toArray();
assert.eq(preImages.length, 0, preImages);

// Perform insert and update operations.
for (const coll of [collA, collB]) {
    assert.commandWorked(coll.insert(docA, {writeConcern: {w: "majority"}}));
    assert.commandWorked(coll.update(docA, {$inc: {version: 1}}));
    assert.commandWorked(coll.update(docB, {$inc: {version: 1}}));
}

// Pre-images collection should contain four pre-images.
preImages = preImagesColl.find().toArray();
assert.eq(preImages.length, 4, preImages);

// Roll over all current oplog entries.
const lastOplogEntryToBeRemoved = getLatestOp(primaryNode);
assert.neq(lastOplogEntryToBeRemoved, null);
const largeStr = 'abcdefghi'.repeat(4 * 1024);

// Helper to check if the oplog has been rolled over from the timestamp of
// 'lastOplogEntryToBeRemoved', ie. the timestamp of the first entry in the oplog is greater than
// the timestamp of the 'lastOplogEntryToBeRemoved'.
function oplogIsRolledOver() {
    return timestampCmp(lastOplogEntryToBeRemoved.ts,
                        getFirstOplogEntry(primaryNode, {readConcern: "majority"}).ts) <= 0;
}

while (!oplogIsRolledOver()) {
    assert.commandWorked(collA.insert({long_str: largeStr}, {writeConcern: {w: "majority"}}));
}

// Perform update operations that inserts new pre-images that are not expired yet.
for (const coll of [collA, collB]) {
    assert.commandWorked(coll.update(docC, {$inc: {version: 1}}));
}

// Wait until 'PeriodicChangeStreamExpiredPreImagesRemover' periodic job will delete the expired
// pre-images.
assert.soon(() => {
    // Only two pre-images should still be there, as their timestamp is greater than the oldest
    // oplog entry timestamp.
    preImages = preImagesColl.find().toArray();
    const onlyTwoPreImagesLeft = preImages.length == 2;
    const allPreImagesHaveBiggerTimestamp = preImages.every(
        preImage => timestampCmp(preImage._id.ts, lastOplogEntryToBeRemoved.ts) == 1);
    return onlyTwoPreImagesLeft && allPreImagesHaveBiggerTimestamp;
});

rst.stopSet();
}());
