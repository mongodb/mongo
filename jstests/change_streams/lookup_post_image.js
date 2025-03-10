// Tests the 'fullDocument' argument to the $changeStream stage.
//
// The $changeStream stage is not allowed within a $facet stage.
// @tags: [
//   do_not_wrap_aggregations_in_facets,
//   uses_multiple_connections,
// ]
(function() {
"use strict";

load("jstests/libs/change_stream_util.js");
load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.
load("jstests/replsets/libs/two_phase_drops.js");  // For 'TwoPhaseDropCollectionTest'.

const coll = assertDropAndRecreateCollection(db, "change_post_image");
const cst = new ChangeStreamTest(db);

jsTestLog("Testing change streams without 'fullDocument' specified");
// Test that not specifying 'fullDocument' does include a 'fullDocument' in the result for
// an insert.
let cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: coll});
assert.commandWorked(coll.insert({_id: "fullDocument not specified"}));
let latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "insert");
assert.eq(latestChange.fullDocument, {_id: "fullDocument not specified"});

// Test that not specifying 'fullDocument' does include a 'fullDocument' in the result for a
// replacement-style update.
assert.commandWorked(coll.update({_id: "fullDocument not specified"},
                                 {_id: "fullDocument not specified", replaced: true}));
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "replace");
assert.eq(latestChange.fullDocument, {_id: "fullDocument not specified", replaced: true});

// Test that not specifying 'fullDocument' does not include a 'fullDocument' in the result
// for a non-replacement update.
assert.commandWorked(coll.update({_id: "fullDocument not specified"}, {$set: {updated: true}}));
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "update");
assert(!latestChange.hasOwnProperty("fullDocument"));

jsTestLog("Testing change streams with 'fullDocument' specified as 'default'");

// Test that specifying 'fullDocument' as 'default' does include a 'fullDocument' in the
// result for an insert.
cursor = cst.startWatchingChanges(
    {collection: coll, pipeline: [{$changeStream: {fullDocument: "default"}}]});
assert.commandWorked(coll.insert({_id: "fullDocument is default"}));
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "insert");
assert.eq(latestChange.fullDocument, {_id: "fullDocument is default"});

// Test that specifying 'fullDocument' as 'default' does include a 'fullDocument' in the
// result for a replacement-style update.
assert.commandWorked(coll.update({_id: "fullDocument is default"},
                                 {_id: "fullDocument is default", replaced: true}));
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "replace");
assert.eq(latestChange.fullDocument, {_id: "fullDocument is default", replaced: true});

// Test that specifying 'fullDocument' as 'default' does not include a 'fullDocument' in the
// result for a non-replacement update.
assert.commandWorked(coll.update({_id: "fullDocument is default"}, {$set: {updated: true}}));
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "update");
assert(!latestChange.hasOwnProperty("fullDocument"));

jsTestLog("Testing change streams with 'fullDocument' specified as 'updateLookup'");

// Test that specifying 'fullDocument' as 'updateLookup' does include a 'fullDocument' in
// the result for an insert.
cursor = cst.startWatchingChanges(
    {collection: coll, pipeline: [{$changeStream: {fullDocument: "updateLookup"}}]});
assert.commandWorked(coll.insert({_id: "fullDocument is lookup"}));
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "insert");
assert.eq(latestChange.fullDocument, {_id: "fullDocument is lookup"});

// Test that specifying 'fullDocument' as 'updateLookup' does include a 'fullDocument' in
// the result for a replacement-style update.
assert.commandWorked(
    coll.update({_id: "fullDocument is lookup"}, {_id: "fullDocument is lookup", replaced: true}));
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "replace");
assert.eq(latestChange.fullDocument, {_id: "fullDocument is lookup", replaced: true});

// Test that specifying 'fullDocument' as 'updateLookup' does include a 'fullDocument' in
// the result for a non-replacement update.
assert.commandWorked(coll.update({_id: "fullDocument is lookup"}, {$set: {updated: true}}));
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "update");
assert.eq(latestChange.fullDocument,
          {_id: "fullDocument is lookup", replaced: true, updated: true});

// Test how a change stream behaves when it is created with 'fullDocument: updateLookup', then a
// document is updated and removed, and then events are retrieved from the change stream.
cursor = cst.startWatchingChanges({
    collection: coll,
    pipeline: [{$changeStream: {fullDocument: "updateLookup"}}, {$match: {operationType: "update"}}]
});
assert.commandWorked(coll.update({_id: "fullDocument is lookup"}, {$set: {updatedAgain: true}}));
assert.commandWorked(coll.remove({_id: "fullDocument is lookup"}));
// If this test is running with secondary read preference, it's necessary for the remove
// to propagate to all secondary nodes and be available for majority reads before we can
// assume looking up the document will fail.
FixtureHelpers.awaitLastOpCommitted(db);

// The next entry is the 'update' operation. Because the corresponding document has been deleted,
// our attempt to look up the post-image results in a null document.
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "update");
assert(latestChange.hasOwnProperty("fullDocument"));
assert.eq(latestChange.fullDocument, null);

const deleteDocResumePoint = latestChange._id;

// Test how a change stream behaves when it is created with 'fullDocument: updateLookup' using a
// resume token from an earlier point in time, then the collection gets dropped, and then events
// are retrieved from the change stream.
assert.commandWorked(coll.insert({_id: "fullDocument is lookup 2"}));
assert.commandWorked(coll.update({_id: "fullDocument is lookup 2"}, {$set: {updated: true}}));

// Open the $changeStream cursor with batchSize 0 so that no change stream events are prefetched
// before the collection is dropped.
cursor = cst.startWatchingChanges({
    collection: coll,
    pipeline: [
        {$changeStream: {resumeAfter: deleteDocResumePoint, fullDocument: "updateLookup"}},
        {$match: {operationType: {$ne: "delete"}}}
    ],
    aggregateOptions: {cursor: {batchSize: 0}}
});

// Drop the collection and wait until two-phase drop finishes.
assertDropCollection(db, coll.getName());
assert.soon(function() {
    return !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(db, coll.getName());
});
// If this test is running with secondary read preference, it's necessary for the drop
// to propagate to all secondary nodes and be available for majority reads before we can
// assume looking up the document will fail.
FixtureHelpers.awaitLastOpCommitted(db);

// Check the next $changeStream entry; this is the test document inserted above.
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "insert");
assert(latestChange.hasOwnProperty("fullDocument"));
assert.eq(latestChange.fullDocument, {_id: "fullDocument is lookup 2"});

// The next entry is the 'update' operation. Because the collection has been dropped, our
// attempt to look up the post-image results in a null document.
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "update");
assert(latestChange.hasOwnProperty("fullDocument"));
assert.eq(latestChange.fullDocument, null);

// After a collection has been dropped, verify that a change stream can be created with
// 'fullDocument: updateLookup' using a resume token from an earlier point in time.
cursor = cst.startWatchingChanges({
    collection: coll,
    pipeline: [
        {$changeStream: {resumeAfter: deleteDocResumePoint, fullDocument: "updateLookup"}},
        {$match: {operationType: {$ne: "delete"}}}
    ],
    aggregateOptions: {cursor: {batchSize: 0}}
});

// The next entry is the 'insert' operation.
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "insert");
assert(latestChange.hasOwnProperty("fullDocument"));
assert.eq(latestChange.fullDocument, {_id: "fullDocument is lookup 2"});

// The next entry is the 'update' operation. Because the collection has been dropped, our
// attempt to look up the post-image results in a null document.
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "update");
assert(latestChange.hasOwnProperty("fullDocument"));
assert.eq(latestChange.fullDocument, null);

// Test how a change stream behaves when a collection is dropped and re-created, then the change
// stream is created with 'fullDocument: updateLookup' using a resume token from before the
// collection was dropped, and then events are retrieved from the change stream.
assertCreateCollection(db, coll.getName());

// Insert a new document with the same _id as the document from the previous incarnation of this
// collection.
assert.commandWorked(coll.insert({_id: "fullDocument is lookup 2"}));

// After a collection has been dropped and re-created, verify a change stream can be created with
// 'fullDocument: updateLookup' using a resume token from before the collection was dropped.
cursor = cst.startWatchingChanges({
    collection: coll,
    pipeline: [
        {$changeStream: {resumeAfter: deleteDocResumePoint, fullDocument: "updateLookup"}},
        {$match: {operationType: {$ne: "delete"}}}
    ],
    aggregateOptions: {cursor: {batchSize: 0}}
});

// The next entry is the 'insert' operation.
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "insert");
assert(latestChange.hasOwnProperty("fullDocument"));
assert.eq(latestChange.fullDocument, {_id: "fullDocument is lookup 2"});

// The next entry is the 'update' operation. Confirm that the next entry's post-image is null
// because the original collection (i.e. the collection that the 'update' was applied to) has
// been dropped and the new incarnation of the collection has a different UUID.
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "update");
assert(latestChange.hasOwnProperty("fullDocument"));
assert.eq(latestChange.fullDocument, null);

jsTestLog("Testing full document lookup with a real getMore");
assert.commandWorked(coll.insert({_id: "getMoreEnabled"}));

cursor = cst.startWatchingChanges({
    collection: coll,
    pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
});
assert.commandWorked(coll.update({_id: "getMoreEnabled"}, {$set: {updated: true}}));

const doc = cst.getOneChange(cursor);
assert.docEq({_id: "getMoreEnabled", updated: true}, doc["fullDocument"]);

// Test that invalidate entries don't have 'fullDocument' even if 'updateLookup' is
// specified.
cursor = cst.startWatchingChanges({
    collection: coll,
    pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
    aggregateOptions: {cursor: {batchSize: 0}}
});
assert.commandWorked(coll.insert({_id: "testing invalidate"}));
assertDropCollection(db, coll.getName());
// Wait until two-phase drop finishes.
assert.soon(function() {
    return !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(db, coll.getName());
});
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "insert");
latestChange = cst.getOneChange(cursor);
assert.eq(latestChange.operationType, "drop");
// Only single-collection change streams will be invalidated by the drop.
if (!isChangeStreamPassthrough()) {
    latestChange = cst.getOneChange(cursor, true);
    assert.eq(latestChange.operationType, "invalidate");
}
}());
