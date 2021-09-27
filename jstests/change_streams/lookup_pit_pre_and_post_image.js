// Tests that pre-images are stored in the pre-images collection on updates in collections with
// 'changeStreamPreAndPostImages' set to true.
// @tags: [
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

if (!isChangeStreamPreAndPostImagesEnabled(db)) {
    // If feature flag is off, creating changeStream with new fullDocument arguments should throw.
    assert.throwsWithCode(() => coll.watch([], {fullDocument: 'whenAvailable'}),
                          ErrorCodes.BadValue);
    assert.throwsWithCode(() => coll.watch([], {fullDocument: 'required'}), ErrorCodes.BadValue);

    jsTestLog(
        'Skipping test because featureFlagChangeStreamPreAndPostImages feature flag is not enabled');
    return;
}

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
const updatedDoc2 = {
    _id: 1,
    x: 5
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

// Open the change streams with new fullDocument parameter set.
assert.doesNotThrow(() => coll.watch([], {fullDocument: 'whenAvailable'}));
assert.doesNotThrow(() => coll.watch([], {fullDocument: 'required'}));

// Open the change streams with fullDocumentBeforeChange parameter set.
const changeStreamCursorWhenAvailable = coll.watch([], {fullDocumentBeforeChange: 'whenAvailable'});
let changeStreamCursorRequired = coll.watch([], {fullDocumentBeforeChange: 'required'});

// Perform an insert.
assert.commandWorked(coll.insert(originalDoc));
assert.soon(() => changeStreamCursorWhenAvailable.hasNext());
assert.soon(() => changeStreamCursorRequired.hasNext());
assert.eq(changeStreamCursorWhenAvailable.next().operationType, 'insert');
assert.eq(changeStreamCursorRequired.next().operationType, 'insert');

// Pre-images collection should remain empty, as pre-images for insert operations can be found in
// the oplog.
assert.eq(preImagesColl.find().count(), 0);

// Perform an update with 'damages'.
assert.commandWorked(coll.update(originalDoc, {$inc: {x: 2}}));

// Since changeStreamPreAndPostImages is not enabled, pre-images collection must be empty.
assert.eq(preImagesColl.find().count(), 0);

// Change stream with { fullDocumentBeforeChange: 'whenAvailable' } should return null as pre-image.
assert.soon(() => changeStreamCursorWhenAvailable.hasNext());
const doc = changeStreamCursorWhenAvailable.next();
assert(doc.hasOwnProperty("fullDocumentBeforeChange"));
assert.isnull(doc.fullDocumentBeforeChange);

// Change stream with { fullDocumentBeforeChange: 'required' } should fail as pre-image is not
// available.
try {
    assert.soon(() => changeStreamCursorRequired.hasNext());
    assert(false, `Unexpected result from cursor: ${tojson(changeStreamCursorRequired.next())}`);
} catch (error) {
    assert.eq(error.code,
              ErrorCodes.ChangeStreamHistoryLost,
              `Caught unexpected error: ${tojson(error)}`);
}

// Enable changeStreamPreAndPostImages for pre-images recording.
assert.commandWorked(testDB.runCommand({collMod: collName, changeStreamPreAndPostImages: true}));

// Reopen the failed change stream.
changeStreamCursorRequired = coll.watch([], {fullDocumentBeforeChange: 'required'});

// Perform an update with 'damages'.
assert.commandWorked(coll.update(updatedDoc, {$inc: {x: 2}}));

// Pre-images collection should contain one document with the 'updatedDoc' pre-image.
assert.eq(preImagesColl.find().count({"preImage": updatedDoc}), 1);
let preImageDoc = preImagesColl.find({"preImage": updatedDoc}).next();
assertValidPreImage(preImageDoc);

// Change stream should contain the pre-image.
assert.soon(() => changeStreamCursorWhenAvailable.hasNext());
assert.soon(() => changeStreamCursorRequired.hasNext());
assert.eq(changeStreamCursorWhenAvailable.next().fullDocumentBeforeChange, updatedDoc);
assert.eq(changeStreamCursorRequired.next().fullDocumentBeforeChange, updatedDoc);

// Perform an update (replace).
assert.commandWorked(coll.update(updatedDoc2, {z: 1}));

// Pre-Images collection should contain a new document with the 'updatedDoc2' pre-image.
assert.eq(preImagesColl.find({"preImage": updatedDoc2}).count(), 1);
preImageDoc = preImagesColl.find({"preImage": updatedDoc2}).next();
assertValidPreImage(preImageDoc);

// Change stream should contain the pre-image.
assert.soon(() => changeStreamCursorWhenAvailable.hasNext());
assert.soon(() => changeStreamCursorRequired.hasNext());

let changeStreamDoc = changeStreamCursorWhenAvailable.next();
assert.eq(changeStreamDoc.fullDocumentBeforeChange, updatedDoc2);
assert.eq(changeStreamDoc.operationType, "replace");

changeStreamDoc = changeStreamCursorRequired.next();
assert.eq(changeStreamDoc.fullDocumentBeforeChange, updatedDoc2);
assert.eq(changeStreamDoc.operationType, "replace");
}());
