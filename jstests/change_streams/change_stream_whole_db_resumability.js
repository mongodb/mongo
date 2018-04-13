// Basic tests for resuming a $changeStream that is open against all collections in a database.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.

    // Drop and recreate the collections to be used in this set of tests.
    const coll = assertDropAndRecreateCollection(db, jsTestName() + "resume_coll");
    const otherColl = assertDropAndRecreateCollection(db, jsTestName() + "resume_coll_other");

    let cst = new ChangeStreamTest(db);
    let resumeCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    // Insert a single document to each collection and save the resume token from the first insert.
    assert.writeOK(coll.insert({_id: 1}));
    assert.writeOK(otherColl.insert({_id: 2}));
    const firstInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(firstInsertChangeDoc.fullDocument, {_id: 1});
    assert.eq(firstInsertChangeDoc.ns, {db: "test", coll: coll.getName()});

    // Test resuming the change stream after the first insert should pick up the insert on the
    // second collection.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });

    const secondInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(secondInsertChangeDoc.fullDocument, {_id: 2});
    assert.eq(secondInsertChangeDoc.ns, {db: "test", coll: otherColl.getName()});

    // Insert a third document to the first collection and test that the change stream picks it up.
    assert.writeOK(coll.insert({_id: 3}));
    const thirdInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(thirdInsertChangeDoc.fullDocument, {_id: 3});
    assert.eq(thirdInsertChangeDoc.ns, {db: "test", coll: coll.getName()});

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
