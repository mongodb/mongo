// Tests that pre-images are stored in the pre-images collection on updates in collections with
// 'changeStreamPreAndPostImages' set to true.
// @tags: [
//  requires_fcv_51,
//  featureFlagChangeStreamPreAndPostImages,
//  assumes_against_mongod_not_mongos,
//  change_stream_does_not_expect_txns,
//  multiversion_incompatible,
// ]
(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.
load("jstests/libs/change_stream_util.js");        // For isChangeStreamPreAndPostImagesEnabled.

const testDB = db.getSiblingDB(jsTestName());
const localDB = db.getSiblingDB("local");
const collName = "test";
const coll = assertDropAndRecreateCollection(testDB, collName);
const collUUID = testDB.getCollectionInfos({name: collName})[0].info.uuid;
const preImagesColl = assertDropAndRecreateCollection(localDB, "system.preimages");
const originalDoc = {
    _id: 1,
    x: 1
};
const updatedDoc = {
    _id: 1,
    x: 3
};

// Validates the contents of the pre-image collection entry.
function assertValidChangeStreamPreImageDocument(preImage) {
    const oplogEntryCursor = localDB.oplog.rs.find({ts: preImage._id.ts});
    assert(oplogEntryCursor.hasNext());
    const oplogEntry = oplogEntryCursor.next();
    assert.eq(oplogEntry.op, "u", oplogEntry);
    assert.eq(preImage._id.nsUUID, oplogEntry.ui);
    assert.eq(preImage._id.nsUUID, collUUID);
    assert.eq(preImage._id.applyOpsIndex, 0);
    assert.eq(preImage.operationTime, oplogEntry.wall, oplogEntry);
    assert.eq(preImage.preImage._id, oplogEntry.o2._id, oplogEntry);
}

// Perform an insert, an update modification and a delete.
assert.commandWorked(coll.insert(originalDoc));
assert.commandWorked(coll.update(originalDoc, {$inc: {x: 2}}));
assert.commandWorked(coll.remove(updatedDoc));

// Since changeStreamPreAndPostImages is not enabled, pre-images collection must be empty.
assert.eq(preImagesColl.count(), 0);

// Enable changeStreamPreAndPostImages for pre-images recording.
assert.commandWorked(testDB.runCommand({collMod: collName, changeStreamPreAndPostImages: true}));

// Perform an insert.
assert.commandWorked(coll.insert(originalDoc));
assert.eq(coll.find().count(), 1);

// Pre-images collection should remain empty, as insert operations do not have pre-images.
assert.eq(preImagesColl.find().count(), 0);

// Perform an update modification.
assert.commandWorked(coll.update(originalDoc, {$inc: {x: 2}}));

// Pre-images collection should contain one document with the 'originalDoc' pre-image.
let preImages = preImagesColl.find({"preImage": originalDoc}).toArray();
assert.eq(preImages.length, 1);
assertValidChangeStreamPreImageDocument(preImages[0]);

// Perform a full-document replacement.
assert.commandWorked(coll.update(updatedDoc, {z: 1}));

// Pre-images collection should contain a new document with the 'updatedDoc' pre-image.
preImages = preImagesColl.find({"preImage": updatedDoc}).toArray();
assert.eq(preImages.length, 1);
assertValidChangeStreamPreImageDocument(preImages[0]);
}());
