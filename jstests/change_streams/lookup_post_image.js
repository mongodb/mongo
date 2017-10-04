// Tests the 'fullDocument' argument to the $changeStream stage.
//
// The $changeStream stage is not allowed within a $facet stage.
// @tags: [do_not_wrap_aggregations_in_facets]
(function() {
    "use strict";

    load("jstests/libs/change_stream_util.js");
    load("jstests/replsets/libs/two_phase_drops.js");  // For 'TwoPhaseDropCollectionTest'.

    let cst = new ChangeStreamTest(db);
    const coll = db.change_post_image;

    coll.drop();

    jsTestLog("Testing change streams without 'fullDocument' specified");
    // Test that not specifying 'fullDocument' does include a 'fullDocument' in the result for an
    // insert.
    let cursor = cst.startWatchingChanges(
        {pipeline: [{$changeStream: {}}], collection: coll, includeTs: true});
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

    // Test that not specifying 'fullDocument' does not include a 'fullDocument' in the result for
    // a non-replacement update.
    assert.writeOK(coll.update({_id: "fullDocument not specified"}, {$set: {updated: true}}));
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "update");
    assert(!latestChange.hasOwnProperty("fullDocument"));

    jsTestLog("Testing change streams with 'fullDocument' specified as 'default'");

    // Test that specifying 'fullDocument' as 'default' does include a 'fullDocument' in the result
    // for an insert.
    cursor = cst.startWatchingChanges({
        collection: coll,
        pipeline: [{$changeStream: {fullDocument: "default"}}],
        includeTs: true
    });
    assert.writeOK(coll.insert({_id: "fullDocument is default"}));
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "insert");
    assert.eq(latestChange.fullDocument, {_id: "fullDocument is default"});

    // Test that specifying 'fullDocument' as 'default' does include a 'fullDocument' in the result
    // for a replacement-style update.
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

    // Test that specifying 'fullDocument' as 'updateLookup' does include a 'fullDocument' in the
    // result for an insert.
    cursor = cst.startWatchingChanges({
        collection: coll,
        pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
        includeTs: true
    });
    assert.writeOK(coll.insert({_id: "fullDocument is lookup"}));
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "insert");
    assert.eq(latestChange.fullDocument, {_id: "fullDocument is lookup"});

    // Test that specifying 'fullDocument' as 'updateLookup' does include a 'fullDocument' in the
    // result for a replacement-style update.
    assert.writeOK(coll.update({_id: "fullDocument is lookup"},
                               {_id: "fullDocument is lookup", replaced: true}));
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "replace");
    assert.eq(latestChange.fullDocument, {_id: "fullDocument is lookup", replaced: true});

    // Test that specifying 'fullDocument' as 'updateLookup' does include a 'fullDocument' in the
    // result for a non-replacement update.
    assert.writeOK(coll.update({_id: "fullDocument is lookup"}, {$set: {updated: true}}));
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "update");
    assert.eq(latestChange.fullDocument,
              {_id: "fullDocument is lookup", replaced: true, updated: true});

    // Test that looking up the post image of an update after deleting the document will result in a
    // 'fullDocument' with a value of null.
    cursor = cst.startWatchingChanges({
        collection: coll,
        pipeline:
            [{$changeStream: {fullDocument: "updateLookup"}}, {$match: {operationType: "update"}}],
        includeTs: true
    });
    assert.writeOK(coll.update({_id: "fullDocument is lookup"}, {$set: {updatedAgain: true}}));
    assert.writeOK(coll.remove({_id: "fullDocument is lookup"}));
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "update");
    assert(latestChange.hasOwnProperty("fullDocument"));
    assert.eq(latestChange.fullDocument, null);
    const deleteDocResumePoint = latestChange._id;

    // Test that looking up the post image of an update after the collection has been dropped will
    // result in 'fullDocument' with a value of null.  This must be done using getMore because new
    // cursors cannot be established after a collection drop.
    assert.writeOK(coll.insert({_id: "fullDocument is lookup 2"}));
    assert.writeOK(coll.update({_id: "fullDocument is lookup 2"}, {$set: {updated: true}}));

    // Open a $changeStream cursor with batchSize 0, so that no oplog entries are retrieved yet.
    cursor = cst.startWatchingChanges({
        collection: coll,
        pipeline: [
            {$changeStream: {fullDocument: "updateLookup", resumeAfter: deleteDocResumePoint}},
            {$match: {operationType: "update"}}
        ],
        aggregateOptions: {cursor: {batchSize: 0}}
    });

    // Save another stream to test post-image lookup after the collection is recreated.
    let cursorBeforeDrop = cst.startWatchingChanges({
        collection: coll,
        pipeline: [
            {$changeStream: {fullDocument: "updateLookup", resumeAfter: deleteDocResumePoint}},
            {$match: {operationType: "update"}}
        ],
        aggregateOptions: {cursor: {batchSize: 0}}
    });

    // Drop the collection and wait until two-phase drop finishes.
    coll.drop();
    assert.soon(function() {
        return !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(db, coll.getName());
    });

    // Check the next $changeStream entry; this is the test document inserted above. The collection
    // has been dropped, so our attempt to look up the post-image results in a null document.
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "update");
    assert(latestChange.hasOwnProperty("fullDocument"));
    assert.eq(latestChange.fullDocument, null);

    // Test establishing new cursors with resume token on dropped collections fails.
    let res = db.runCommand({
        aggregate: coll.getName(),
        pipeline: [
            {$changeStream: {fullDocument: "updateLookup", resumeAfter: deleteDocResumePoint}},
            {$match: {operationType: "update"}}
        ],
        cursor: {batchSize: 0}
    });
    assert.commandFailedWithCode(res, 40615);

    // Test that looking up the post image of an update after the collection has been dropped and
    // created again will result in 'fullDocument' with a value of null. This must be done using
    // getMore because new cursors cannot be established after a collection drop.
    //
    // Insert a document with the same _id, verify the change stream won't return it due to
    // different UUID.
    assert.commandWorked(db.createCollection(coll.getName()));
    assert.writeOK(coll.insert({_id: "fullDocument is lookup 2"}));

    // Confirm that the next entry's post-image is null since new collection has a different UUID.
    latestChange = cst.getOneChange(cursorBeforeDrop);
    assert.eq(latestChange.operationType, "update");
    assert(latestChange.hasOwnProperty("fullDocument"));
    assert.eq(latestChange.fullDocument, null);

    // Test that invalidate entries don't have 'fullDocument' even if 'updateLookup' is specified.
    db.collInvalidate.drop();
    assert.commandWorked(db.createCollection(db.collInvalidate.getName()));
    cursor = cst.startWatchingChanges({
        collection: db.collInvalidate,
        pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
        aggregateOptions: {cursor: {batchSize: 0}}
    });
    assert.writeOK(db.collInvalidate.insert({_id: "testing invalidate"}));
    db.collInvalidate.drop();
    // Wait until two-phase drop finishes.
    assert.soon(function() {
        return !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(
            db, db.collInvalidate.getName());
    });
    latestChange = cst.getOneChange(cursor);
    assert.eq(latestChange.operationType, "insert");
    latestChange = cst.getOneChange(cursor, true);
    assert.eq(latestChange.operationType, "invalidate");
    assert(!latestChange.hasOwnProperty("fullDocument"));

    // TODO(russotto): Can just use "coll" here once read majority is working.
    // For now, using the old collection results in us reading stale data sometimes.
    jsTestLog("Testing full document lookup with a real getMore");
    const coll2 = db.real_get_more;
    coll2.drop();
    assert.commandWorked(db.createCollection(coll2.getName()));
    assert.writeOK(coll2.insert({_id: "getMoreEnabled"}));

    cursor = cst.startWatchingChanges({
        collection: coll2,
        pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
    });
    assert.writeOK(coll2.update({_id: "getMoreEnabled"}, {$set: {updated: true}}));

    let doc = cst.getOneChange(cursor);
    assert.docEq(doc["fullDocument"], {_id: "getMoreEnabled", updated: true});

    cst.cleanUp();
}());
