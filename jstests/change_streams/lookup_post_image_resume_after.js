// Tests the behavior of using fullDocument: "updateLookup" with a resumeToken, possibly from far
// enough in the past that the document doesn't exist yet.
// @tags: [uses_resume_after]
(function() {
    "use strict";

    load("jstests/libs/change_stream_util.js");
    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/fixture_helpers.js");           // For awaitLastOpCommitted().
    load("jstests/replsets/libs/two_phase_drops.js");  // For 'TwoPhaseDropCollectionTest'.

    let cst = new ChangeStreamTest(db);
    const coll = assertDropAndRecreateCollection(db, "post_image_resume_after");
    const streamToGetResumeToken = cst.startWatchingChanges({
        collection: coll,
        pipeline: [{$changeStream: {}}],
        includeToken: true,
        aggregateOptions: {cursor: {batchSize: 0}}
    });
    assert.writeOK(coll.insert({_id: "for resuming later"}));
    const resumePoint = cst.getOneChange(streamToGetResumeToken)._id;

    // Test that looking up the post image of an update after the collection has been dropped will
    // result in 'fullDocument' with a value of null. This must be done using getMore because new
    // cursors cannot be established after a collection drop.
    assert.writeOK(coll.insert({_id: "TARGET"}));
    assert.writeOK(coll.update({_id: "TARGET"}, {$set: {updated: true}}));

    // Open a $changeStream cursor with batchSize 0, so that no oplog entries are retrieved yet.
    const firstResumeCursor = cst.startWatchingChanges({
        collection: coll,
        pipeline: [{$changeStream: {fullDocument: "updateLookup", resumeAfter: resumePoint}}],
        aggregateOptions: {cursor: {batchSize: 0}}
    });

    // Save another stream to test post-image lookup after the collection is recreated. We have to
    // create this before we drop the collection because otherwise the resume token's UUID will not
    // match that of the collection.
    const secondResumeCursor = cst.startWatchingChanges({
        collection: coll,
        pipeline: [{$changeStream: {fullDocument: "updateLookup", resumeAfter: resumePoint}}],
        aggregateOptions: {cursor: {batchSize: 0}}
    });

    // Drop the collection and wait until two-phase drop finishes.
    assertDropCollection(db, coll.getName());
    assert.soon(
        () => !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(db, coll.getName()));

    // If this test is running with secondary read preference, it's necessary for the drop to
    // propagate to all secondary nodes and be available for majority reads before we can assume
    // looking up the document will fail.
    FixtureHelpers.awaitLastOpCommitted();

    // Check the next $changeStream entry; this is the test document inserted above.
    let latestChange = cst.getOneChange(firstResumeCursor);
    assert.eq(latestChange.operationType, "insert");
    assert.eq(latestChange.fullDocument, {_id: "TARGET"});

    // The next entry is the 'update' operation. Because the collection has been dropped, our
    // attempt to look up the post-image results in a null document.
    latestChange = cst.getOneChange(firstResumeCursor);
    assert.eq(latestChange.operationType, "update");
    assert(latestChange.hasOwnProperty("fullDocument"));
    assert.eq(latestChange.fullDocument, null);

    // Test establishing new cursors with resume token on dropped collections fails.
    assert.commandFailedWithCode(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [
            {$changeStream: {fullDocument: "updateLookup", resumeAfter: resumePoint}},
            {$match: {operationType: "update"}}
        ],
        cursor: {batchSize: 0}
    }),
                                 40615);

    // Before we re-create the collection we must consume the insert notification. This is to
    // prevent the change stream from throwing an assertion when it goes to look up the shard key
    // for the collection and finds that it has a mismatching UUID. It should proceed without error
    // if the collection doesn't exist (has no UUID).
    cst.assertNextChangesEqual({
        cursor: secondResumeCursor,
        expectedChanges: [{
            documentKey: {_id: "TARGET"},
            fullDocument: {_id: "TARGET"},
            ns: {db: db.getName(), coll: coll.getName()},
            operationType: "insert"
        }]
    });

    // Recreate the collection, insert a document with the same _id, and test that the change stream
    // won't return it because the collection will now have a different UUID.
    assert.writeOK(coll.insert({_id: "fullDocument is lookup 2"}));

    cst.assertNextChangesEqual({
        cursor: secondResumeCursor,
        expectedChanges: [{
            documentKey: {_id: "TARGET"},
            fullDocument: null,
            ns: {db: db.getName(), coll: coll.getName()},
            operationType: "update",
            updateDescription: {updatedFields: {updated: true}, removedFields: []}
        }]
    });

    cst.cleanUp();
}());
