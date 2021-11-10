// Tests that pre-images are stored in the pre-images collection on updates and deletes in
// collections with 'changeStreamPreAndPostImages' being enabled.
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

load("jstests/libs/collection_drop_recreate.js");  // For assertDropCollection,
                                                   // assertDropAndRecreateCollection.

const testDB = db.getSiblingDB(jsTestName());
const localDB = db.getSiblingDB("local");
const configDB = db.getSiblingDB("config");
const collName = "test";
const preImagesCollName = "system.preimages";
const originalDoc = {
    _id: 1,
    x: 1
};
const updatedDoc = {
    _id: 1,
    x: 3
};
const replacedDoc = {
    _id: 1,
    z: 1
};

// Validates the contents of the pre-image collection entry.
function assertValidChangeStreamPreImageDocument(preImage) {
    const oplogEntryCursor = localDB.oplog.rs.find({ts: preImage._id.ts});
    assert(oplogEntryCursor.hasNext());
    const oplogEntry = oplogEntryCursor.next();

    // Pre-images documents are recorded only for update and delete commands.
    assert.contains(oplogEntry.op, ["u", "d"], oplogEntry);
    assert.eq(preImage._id.nsUUID, oplogEntry.ui);
    assert.eq(preImage._id.applyOpsIndex, 0);
    assert.eq(preImage.operationTime, oplogEntry.wall, oplogEntry);
    if (oplogEntry.hasOwnProperty("o2")) {
        assert.eq(preImage.preImage._id, oplogEntry.o2._id, oplogEntry);
    }
}

function testFunc(collOptions = {}) {
    let coll = assertDropAndRecreateCollection(testDB, collName, collOptions);
    assertDropCollection(configDB, preImagesCollName);
    const preImagesColl = configDB.getCollection(preImagesCollName);

    // Perform an insert and an update modification.
    assert.commandWorked(coll.insert(originalDoc));
    assert.commandWorked(coll.update(originalDoc, {$inc: {x: 2}}));
    assert.eq(coll.find().count(), 1);

    // Perform a document removal. If the collection is capped, document removal is not
    // allowed, perform the collection drop instead.
    if (!collOptions.capped) {
        assert.commandWorked(coll.remove(updatedDoc));
    } else {
        coll = assertDropAndRecreateCollection(testDB, collName, collOptions);
    }
    assert.eq(coll.find().count(), 0);

    // Since changeStreamPreAndPostImages is not enabled, pre-images collection must be empty.
    assert.eq(preImagesColl.count(), 0);

    // Enable changeStreamPreAndPostImages for pre-images recording.
    assert.commandWorked(
        testDB.runCommand({collMod: collName, changeStreamPreAndPostImages: {enabled: true}}));

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

    if (collOptions.capped) {
        // Trigger a delete operation from the capped collection by inserting a new document.
        assert.commandWorked(coll.insert({x: "abcd"}));
    } else {
        // Perform a document removal.
        assert.commandWorked(coll.remove(replacedDoc));
    }

    // Pre-images collection should contain a new document with the 'replacedDoc' pre-image.
    preImages = preImagesColl.find({"preImage": replacedDoc}).toArray();
    assert.eq(preImages.length, 1);
    assertValidChangeStreamPreImageDocument(preImages[0]);
}

testFunc();
testFunc({capped: true, size: 1, max: 1});
}());
