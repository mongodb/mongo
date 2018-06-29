// Tests the ability to resume a change stream at different points in the stream.
// @tags: [uses_resume_after]
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");

    let cst = new ChangeStreamTest(db);
    assertDropAndRecreateCollection(db, "resume_after");

    // Note we do not project away 'id.ts' as it is part of the resume token.
    let resumeCursor = cst.startWatchingChanges(
        {pipeline: [{$changeStream: {}}], collection: db.resume_after, includeToken: true});

    // Insert a document and save the resulting change stream.
    assert.writeOK(db.resume_after.insert({_id: 1}));
    const firstInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(firstInsertChangeDoc.fullDocument, {_id: 1});

    jsTestLog("Testing resume after one document.");
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id}}],
        collection: db.resume_after,
        includeToken: true,
        aggregateOptions: {cursor: {batchSize: 0}},
    });

    jsTestLog("Inserting additional documents.");
    assert.writeOK(db.resume_after.insert({_id: 2}));
    const secondInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(secondInsertChangeDoc.fullDocument, {_id: 2});
    assert.writeOK(db.resume_after.insert({_id: 3}));
    const thirdInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(thirdInsertChangeDoc.fullDocument, {_id: 3});

    jsTestLog("Testing resume after first document of three.");
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id}}],
        collection: db.resume_after,
        includeToken: true,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    assert.docEq(cst.getOneChange(resumeCursor), secondInsertChangeDoc);
    assert.docEq(cst.getOneChange(resumeCursor), thirdInsertChangeDoc);

    jsTestLog("Testing resume after second document of three.");
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: secondInsertChangeDoc._id}}],
        collection: db.resume_after,
        includeToken: true,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    assert.docEq(cst.getOneChange(resumeCursor), thirdInsertChangeDoc);

    cst.cleanUp();
}());
