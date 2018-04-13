// Basic tests for resuming a $changeStream that is open against all databases in a cluster.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.

    // Create two databases, with one collection in each.
    const testDBs = [db, db.getSiblingDB(jsTestName() + "_other")];
    const[db1Coll, db2Coll] = testDBs.map((db) => assertDropAndRecreateCollection(db, "test"));
    const adminDB = db.getSiblingDB("admin");

    let cst = new ChangeStreamTest(adminDB);
    let resumeCursor = cst.startWatchingAllChangesForCluster();

    // Insert a document in the first database and save the resulting change stream.
    assert.writeOK(db1Coll.insert({_id: 1}));
    const firstInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(firstInsertChangeDoc.fullDocument, {_id: 1});

    // Test resume after the first insert.
    resumeCursor = cst.startWatchingChanges({
        pipeline:
            [{$changeStream: {resumeAfter: firstInsertChangeDoc._id, allChangesForCluster: true}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });

    // Write the next document into the second database.
    assert.writeOK(db2Coll.insert({_id: 2}));
    const secondInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(secondInsertChangeDoc.fullDocument, {_id: 2});

    // Write the third document into the first database again.
    assert.writeOK(db1Coll.insert({_id: 3}));
    const thirdInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(thirdInsertChangeDoc.fullDocument, {_id: 3});

    // Test resuming after the first insert again.
    resumeCursor = cst.startWatchingChanges({
        pipeline:
            [{$changeStream: {resumeAfter: firstInsertChangeDoc._id, allChangesForCluster: true}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    assert.docEq(cst.getOneChange(resumeCursor), secondInsertChangeDoc);
    assert.docEq(cst.getOneChange(resumeCursor), thirdInsertChangeDoc);

    // Test resume after second insert.
    resumeCursor = cst.startWatchingChanges({
        pipeline:
            [{$changeStream: {resumeAfter: secondInsertChangeDoc._id, allChangesForCluster: true}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    assert.docEq(cst.getOneChange(resumeCursor), thirdInsertChangeDoc);

    cst.cleanUp();
})();
