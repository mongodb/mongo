// Basic tests for resuming a $changeStream that is open against all collections in a database.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.

    assertDropAndRecreateCollection(db, "resumeColl");
    const coll = db.resumeColl;

    // Note we do not project away 'id.ts' as it is part of the resume token.
    let cst = new ChangeStreamTest(db);
    let resumeCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    // Insert a document and save the resulting change stream.
    assert.writeOK(coll.insert({_id: 1}));
    const firstInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(firstInsertChangeDoc.fullDocument, {_id: 1});

    // Test resume after an insert.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });

    assert.writeOK(coll.insert({_id: 2}));
    const secondInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(secondInsertChangeDoc.fullDocument, {_id: 2});
    assert.writeOK(coll.insert({_id: 3}));
    const thirdInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(thirdInsertChangeDoc.fullDocument, {_id: 3});

    // Test resuming after the first insert again.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    assert.docEq(cst.getOneChange(resumeCursor), secondInsertChangeDoc);
    assert.docEq(cst.getOneChange(resumeCursor), thirdInsertChangeDoc);

    // Test resume after second insert.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: secondInsertChangeDoc._id}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    assert.docEq(cst.getOneChange(resumeCursor), thirdInsertChangeDoc);

    cst.cleanUp();
})();
