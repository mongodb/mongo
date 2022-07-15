// Tests that pre-images are stored in the pre-images collection on updates and deletes in
// collections with 'changeStreamPreAndPostImages' being enabled.
// @tags: [
//  requires_fcv_60,
//  assumes_against_mongod_not_mongos,
//  requires_capped,
//  requires_replication,
//  requires_getmore,
//  no_selinux,
// ]
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.isReplSet().
load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.
load(
    "jstests/libs/change_stream_util.js");  // For
                                            // assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled,
                                            // assertChangeStreamPreAndPostImagesCollectionOptionIsAbsent,
                                            // preImagesForOps.

// Pre-images are only recorded in the replica set mode.
if (!FixtureHelpers.isReplSet(db)) {
    return;
}

const testDB = db.getSiblingDB(jsTestName());
const localDB = db.getSiblingDB("local");
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

// Validates that the expected pre-images are written while performing ops.
function assertPreImagesWrittenForOps(db, ops, expectedPreImages) {
    const writtenPreImages = preImagesForOps(db, ops);
    assert.eq(writtenPreImages.length, expectedPreImages.length, writtenPreImages);

    for (let idx = 0; idx < writtenPreImages.length; idx++) {
        assert.eq(writtenPreImages[idx].preImage, expectedPreImages[idx]);
        assertValidChangeStreamPreImageDocument(writtenPreImages[idx]);
    }

    // Because the pre-images collection is implicitly replicated, validate that writes do not
    // generate oplog entries, with the exception of deletions.
    assert.eq(0,
              localDB.oplog.rs.find({op: {'$ne': 'd'}, ns: 'config.system.preimages'}).itcount());
}

// Validates that no pre-image is written while performing ops.
function assertNoPreImageWrittenForOps(db, ops) {
    assertPreImagesWrittenForOps(db, ops, []);
}

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

// Tests the pre-images recording behavior in capped collections.
function testPreImageRecordingInCappedCollection({updateDocFunc, replaceDocFunc}) {
    const collWithPreImages = assertDropAndRecreateCollection(
        testDB,
        "coll_with_pre_images",
        {changeStreamPreAndPostImages: {enabled: true}, capped: true, size: 1, max: 1});
    assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled(testDB,
                                                                collWithPreImages.getName());
    const collWithNoPreImages = assertDropAndRecreateCollection(
        testDB, "coll_with_no_pre_images", {capped: true, size: 1, max: 1});
    assertChangeStreamPreAndPostImagesCollectionOptionIsAbsent(testDB,
                                                               collWithNoPreImages.getName());

    // Verify that no pre-image is recorded, when performing writes to the collection, that doesn't
    // have 'changeStreamPreAndPostImages' enabled.
    assertNoPreImageWrittenForOps(testDB, function() {
        // Perform the insert, update and replace commands.
        assert.commandWorked(collWithNoPreImages.insert(originalDoc));
        updateDocFunc(collWithNoPreImages);
        assert.eq(collWithNoPreImages.find().itcount(), 1);

        // Document removal is not allowed on a capped collection, perform an insert command
        // instead.
        assert.commandWorked(collWithNoPreImages.insert({x: "abcd"}));
    });

    // Verify that no pre-image is recorded, when performing an insert command on the collection,
    // that has 'changeStreamPreAndPostImages' enabled.
    assertNoPreImageWrittenForOps(testDB, function() {
        assert.commandWorked(collWithPreImages.insert(originalDoc));
        assert.eq(collWithPreImages.find().itcount(), 1);
    });

    // Verify that one pre-image is recorded, when performing an update command on the collection,
    // that has 'changeStreamPreAndPostImages' enabled.
    assertPreImagesWrittenForOps(testDB, function() {
        // Perform an update modification.
        updateDocFunc(collWithPreImages);
    }, [originalDoc]);

    // Verify that one pre-image is recorded, when performing a replace command on the collection,
    // that has 'changeStreamPreAndPostImages' enabled.
    assertPreImagesWrittenForOps(testDB, function() {
        // Perform a full-document replacement.
        replaceDocFunc(collWithPreImages);
    }, [updatedDoc]);

    // Verify that one pre-image is recorded, when performing a delete command on the collection,
    // that has 'changeStreamPreAndPostImages' enabled.
    assertPreImagesWrittenForOps(testDB, function() {
        // Trigger a delete operation from the capped collection by inserting a new document.
        assert.commandWorked(collWithPreImages.insert({x: "abcd"}));
    }, [replacedDoc]);
}

// Tests the pre-images recording behavior in non-capped collections.
function testPreImageRecordingInNonCappedCollection(
    {updateDocFunc, replaceDocFunc, removeDocFunc}) {
    const collWithPreImages = assertDropAndRecreateCollection(
        testDB, "coll_with_pre_images", {changeStreamPreAndPostImages: {enabled: true}});
    assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled(testDB,
                                                                collWithPreImages.getName());
    const collWithNoPreImages = assertDropAndRecreateCollection(testDB, "coll_with_no_pre_images");
    assertChangeStreamPreAndPostImagesCollectionOptionIsAbsent(testDB,
                                                               collWithNoPreImages.getName());

    // Verify that no pre-image is recorded, when performing writes to the collection, that doesn't
    // have 'changeStreamPreAndPostImages' enabled.
    assertNoPreImageWrittenForOps(testDB, function() {
        // Perform the insert, update, replace and delete commands.
        assert.commandWorked(collWithNoPreImages.insert(originalDoc));
        updateDocFunc(collWithNoPreImages);
        replaceDocFunc(collWithNoPreImages);
        assert.eq(collWithNoPreImages.find().itcount(), 1);
        removeDocFunc(collWithNoPreImages);
    });

    // Verify that no pre-image is recorded, when performing an insert command into the collection,
    // that has 'changeStreamPreAndPostImages' enabled.
    assertNoPreImageWrittenForOps(testDB, function() {
        assert.commandWorked(collWithPreImages.insert(originalDoc));
        assert.eq(collWithPreImages.find().itcount(), 1);
    });

    // Verify that one pre-image is recorded, when performing an update command on the collection,
    // that has 'changeStreamPreAndPostImages' enabled.
    assertPreImagesWrittenForOps(testDB, function() {
        // Perform an update modification.
        updateDocFunc(collWithPreImages);
    }, [originalDoc]);

    // Verify that one pre-image is recorded, when performing a replace command on the collection,
    // that has 'changeStreamPreAndPostImages' enabled.
    assertPreImagesWrittenForOps(testDB, function() {
        // Perform a full-document replacement.
        replaceDocFunc(collWithPreImages);
    }, [updatedDoc]);

    // Verify that one pre-image is recorded, when performing a delete command on the collection,
    // that has 'changeStreamPreAndPostImages' enabled.
    assertPreImagesWrittenForOps(testDB, function() {
        // Perform a document removal.
        removeDocFunc(collWithPreImages);
    }, [replacedDoc]);
}

// Tests that pre-images are recorded correctly for both capped and non-capped collections.
function testPreImageRecording(modificationOps) {
    // Non-capped collection test.
    testPreImageRecordingInNonCappedCollection(modificationOps);

    // Capped collection test.
    testPreImageRecordingInCappedCollection(modificationOps);
}

// Pre-images must be recorded for update (modify), update (replace) and remove commands.
testPreImageRecording({
    updateDocFunc: function(coll) {
        assert.commandWorked(coll.update(originalDoc, {$inc: {x: 2}}));
    },
    replaceDocFunc: function(coll) {
        assert.commandWorked(coll.update(updatedDoc, {z: 1}));
    },
    removeDocFunc: function(coll) {
        assert.commandWorked(coll.deleteOne(replacedDoc));
    }
});

// Pre-images must be recorded for "findAndModify" commands, while returning the pre-images.
testPreImageRecording({
    updateDocFunc: function(coll) {
        assert.eq(coll.findAndModify({update: {$inc: {x: 2}}, new: false}), originalDoc);
    },
    replaceDocFunc: function(coll) {
        assert.eq(coll.findAndModify({update: replacedDoc, new: false}), updatedDoc);
    },
    removeDocFunc: function(coll) {
        assert.eq(coll.findAndModify({remove: true, new: false}), replacedDoc);
    }
});

// Pre-images must be recorded for "findAndModify" commands, while returning the post-images.
testPreImageRecording({
    updateDocFunc: function(coll) {
        assert.eq(coll.findAndModify({update: {$inc: {x: 2}}, new: true}), updatedDoc);
    },
    replaceDocFunc: function(coll) {
        assert.eq(coll.findAndModify({update: replacedDoc, new: true}), replacedDoc);
    },
    removeDocFunc: function(coll) {
        assert.eq(coll.findAndModify({remove: true}), replacedDoc);
    }
});
}());
