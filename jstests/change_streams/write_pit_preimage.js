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
const coll =
    assertDropAndRecreateCollection(testDB, collName, {changeStreamPreAndPostImages: true});
const collInfos = testDB.getCollectionInfos({name: collName});
assert.eq(collInfos.length, 1);
const collUUID = collInfos[0].info.uuid;
const preImagesColl = assertDropAndRecreateCollection(localDB, "system.preimages");
const originalDoc = {
    _id: 1,
    x: 1
};
const updatedDoc = {
    _id: 1,
    x: 3
};

function assertValidPreImage(preImage) {
    const MAX_TIME_DELTA_SECONDS = 300;  // 5 minutes delay.
    assert.eq(preImage._id.nsUUID, collUUID);
    assert.lte(Math.abs(new Date().getTime() / 1000 - preImage._id.ts.getTime()),
               MAX_TIME_DELTA_SECONDS);
    assert.lte(Math.abs(new Date().getTime() / 1000 - preImage.operationTime.getTime() / 1000),
               MAX_TIME_DELTA_SECONDS);
    assert.eq(preImage._id.applyOpsIndex, 0);
}

// Perform an insert.
assert.commandWorked(coll.insert(originalDoc));
assert.eq(coll.find().count(), 1);

// Pre-images collection should remain empty, as pre-images for insert operations can be found in
// the oplog.
assert.eq(preImagesColl.find().count(), 0);

// Perform an update with 'damages'.
assert.commandWorked(coll.update(originalDoc, {$inc: {x: 2}}));

// Pre-images collection should contain one document with the 'originalDoc' pre-image.
let preimages = preImagesColl.find({"preImage": originalDoc}).toArray();
assert.eq(preimages.length, 1);
assertValidPreImage(preimages[0]);

// Perform an update (replace).
assert.commandWorked(coll.update(updatedDoc, {z: 1}));

// Pre-images collection should contain a new document with the 'updatedDoc' pre-image.
preimages = preImagesColl.find({"preImage": updatedDoc}).toArray();
assert.eq(preimages.length, 1);
assertValidPreImage(preimages[0]);
}());
