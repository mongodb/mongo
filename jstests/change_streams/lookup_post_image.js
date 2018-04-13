// Tests the 'fullDocument' argument to the $changeStream stage.
//
// The $changeStream stage is not allowed within a $facet stage.
// @tags: [do_not_wrap_aggregations_in_facets]
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
    assert.writeOK(coll.insert({_id: "fullDocument not specified"}));
    let latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "insert");
    assert.eq(latestChange.fullDocument, {_id: "fullDocument not specified"});

    // Test that not specifying 'fullDocument' does include a 'fullDocument' in the result for a
    // replacement-style update.
    assert.writeOK(coll.update({_id: "fullDocument not specified"},
                               {_id: "fullDocument not specified", replaced: true}));
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "replace");
    assert.eq(latestChange.fullDocument, {_id: "fullDocument not specified", replaced: true});

    // Test that not specifying 'fullDocument' does not include a 'fullDocument' in the result
    // for a non-replacement update.
    assert.writeOK(coll.update({_id: "fullDocument not specified"}, {$set: {updated: true}}));
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "update");
    assert(!latestChange.hasOwnProperty("fullDocument"));

    jsTestLog("Testing change streams with 'fullDocument' specified as 'default'");

    // Test that specifying 'fullDocument' as 'default' does include a 'fullDocument' in the
    // result for an insert.
    cursor = cst.startWatchingChanges(
        {collection: coll, pipeline: [{$changeStream: {fullDocument: "default"}}]});
    assert.writeOK(coll.insert({_id: "fullDocument is default"}));
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "insert");
    assert.eq(latestChange.fullDocument, {_id: "fullDocument is default"});

    // Test that specifying 'fullDocument' as 'default' does include a 'fullDocument' in the
    // result for a replacement-style update.
    assert.writeOK(coll.update({_id: "fullDocument is default"},
                               {_id: "fullDocument is default", replaced: true}));
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "replace");
    assert.eq(latestChange.fullDocument, {_id: "fullDocument is default", replaced: true});

    // Test that specifying 'fullDocument' as 'default' does not include a 'fullDocument' in the
    // result for a non-replacement update.
    assert.writeOK(coll.update({_id: "fullDocument is default"}, {$set: {updated: true}}));
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "update");
    assert(!latestChange.hasOwnProperty("fullDocument"));

    jsTestLog("Testing change streams with 'fullDocument' specified as 'updateLookup'");

    // Test that specifying 'fullDocument' as 'updateLookup' does include a 'fullDocument' in
    // the result for an insert.
    cursor = cst.startWatchingChanges(
        {collection: coll, pipeline: [{$changeStream: {fullDocument: "updateLookup"}}]});
    assert.writeOK(coll.insert({_id: "fullDocument is lookup"}));
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "insert");
    assert.eq(latestChange.fullDocument, {_id: "fullDocument is lookup"});

    // Test that specifying 'fullDocument' as 'updateLookup' does include a 'fullDocument' in
    // the result for a replacement-style update.
    assert.writeOK(coll.update({_id: "fullDocument is lookup"},
                               {_id: "fullDocument is lookup", replaced: true}));
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "replace");
    assert.eq(latestChange.fullDocument, {_id: "fullDocument is lookup", replaced: true});

    // Test that specifying 'fullDocument' as 'updateLookup' does include a 'fullDocument' in
    // the result for a non-replacement update.
    assert.writeOK(coll.update({_id: "fullDocument is lookup"}, {$set: {updated: true}}));
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "update");
    assert.eq(latestChange.fullDocument,
              {_id: "fullDocument is lookup", replaced: true, updated: true});

    // Test that looking up the post image of an update after deleting the document will result
    // in a 'fullDocument' with a value of null.
    cursor = cst.startWatchingChanges({
        collection: coll,
        pipeline: [
            {$changeStream: {fullDocument: "updateLookup"}},
            {$match: {operationType: "update"}}
        ]
    });
    assert.writeOK(coll.update({_id: "fullDocument is lookup"}, {$set: {updatedAgain: true}}));
    assert.writeOK(coll.remove({_id: "fullDocument is lookup"}));
    // If this test is running with secondary read preference, it's necessary for the remove
    // to propagate to all secondary nodes and be available for majority reads before we can
    // assume looking up the document will fail.
    FixtureHelpers.awaitLastOpCommitted(db);

    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "update");
    assert(latestChange.hasOwnProperty("fullDocument"));
    assert.eq(latestChange.fullDocument, null);
    const deleteDocResumePoint = latestChange._id;

    // Test that looking up the post image of an update after the collection has been dropped
    // will result in 'fullDocument' with a value of null.  This must be done using getMore
    // because new cursors cannot be established after a collection drop.
    assert.writeOK(coll.insert({_id: "fullDocument is lookup 2"}));
    assert.writeOK(coll.update({_id: "fullDocument is lookup 2"}, {$set: {updated: true}}));

    // Open a $changeStream cursor with batchSize 0, so that no oplog entries are retrieved yet.
    cursor = cst.startWatchingChanges({
        collection: coll,
        pipeline: [
            {$changeStream: {fullDocument: "updateLookup", resumeAfter: deleteDocResumePoint}},
            {$match: {operationType: {$ne: "delete"}}}
        ],
        aggregateOptions: {cursor: {batchSize: 0}}
    });

    // Save another stream to test post-image lookup after the collection is recreated.
    const cursorBeforeDrop = cst.startWatchingChanges({
        collection: coll,
        pipeline: [
            {$changeStream: {fullDocument: "updateLookup", resumeAfter: deleteDocResumePoint}},
            {$match: {operationType: {$ne: "delete"}}}
        ],
        aggregateOptions: {cursor: {batchSize: 0}}
    });

    // Retrieve the 'insert' operation from the latter stream. This is necessary on a sharded
    // collection so that the documentKey is retrieved before the collection is recreated;
    // otherwise, per SERVER-31691, a uassert will occur.
    latestChange = cst.getOneChange(cursorBeforeDrop);
    assert.eq(latestChange.operationType, "insert");
    assert(latestChange.hasOwnProperty("fullDocument"));
    assert.eq(latestChange.fullDocument, {_id: "fullDocument is lookup 2"});

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

    // Test that we can resume a change stream with 'fullDocument: updateLookup' after the
    // collection has been dropped. This is only allowed if an explicit collation is provided.
    cursor = cst.startWatchingChanges({
        collection: coll,
        pipeline: [
            {$changeStream: {resumeAfter: deleteDocResumePoint, fullDocument: "updateLookup"}},
            {$match: {operationType: {$ne: "delete"}}}
        ],
        aggregateOptions: {cursor: {batchSize: 0}, collation: {locale: "simple"}}
    });

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

    // Test that looking up the post image of an update after the collection has been dropped
    // and created again will result in 'fullDocument' with a value of null. This must be done
    // using getMore because new cursors cannot be established after a collection drop.

    // Insert a document with the same _id, verify the change stream won't return it due to
    // different UUID.
    assertCreateCollection(db, coll.getName());
    assert.writeOK(coll.insert({_id: "fullDocument is lookup 2"}));

    // Confirm that the next entry's post-image is null since new collection has a different
    // UUID.
    latestChange = cst.getOneChange(cursorBeforeDrop);
    assert.eq(latestChange.operationType, "update");
    assert(latestChange.hasOwnProperty("fullDocument"));
    assert.eq(latestChange.fullDocument, null);

    jsTestLog("Testing full document lookup with a real getMore");
    assert.writeOK(coll.insert({_id: "getMoreEnabled"}));

    cursor = cst.startWatchingChanges({
        collection: coll,
        pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
    });
    assert.writeOK(coll.update({_id: "getMoreEnabled"}, {$set: {updated: true}}));

    const doc = cst.getOneChange(cursor);
    assert.docEq(doc["fullDocument"], {_id: "getMoreEnabled", updated: true});

    // Test that invalidate entries don't have 'fullDocument' even if 'updateLookup' is
    // specified.
    cursor = cst.startWatchingChanges({
        collection: coll,
        pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
        aggregateOptions: {cursor: {batchSize: 0}}
    });
    assert.writeOK(coll.insert({_id: "testing invalidate"}));
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
