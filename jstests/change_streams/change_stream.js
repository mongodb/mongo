// Basic $changeStream tests.
(function() {
    "use strict";

    load("jstests/libs/change_stream_util.js");
    load('jstests/libs/uuid_util.js');

    let cst = new ChangeStreamTest(db);

    jsTestLog("Testing single insert");
    db.t1.drop();
    let cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    // Test that if there are no changes, we return an empty batch.
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    assert.writeOK(db.t1.insert({_id: 0, a: 1}));
    const t1Uuid = getUUIDFromListCollections(db, db.t1.getName());
    let expected = {
        _id: {
            documentKey: {_id: 0},
            uuid: t1Uuid,
        },
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 1},
        ns: {db: "test", coll: "t1"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Test that if there are no changes during a subsequent 'getMore', we return an empty batch.
    cursor = cst.getNextBatch(cursor);
    assert.eq(0, cursor.nextBatch.length, "Cursor had changes: " + tojson(cursor));

    jsTestLog("Testing second insert");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    assert.writeOK(db.t1.insert({_id: 1, a: 2}));
    expected = {
        _id: {
            documentKey: {_id: 1},
            uuid: t1Uuid,
        },
        documentKey: {_id: 1},
        fullDocument: {_id: 1, a: 2},
        ns: {db: "test", coll: "t1"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing update");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    assert.writeOK(db.t1.update({_id: 0}, {a: 3}));
    expected = {
        _id: {documentKey: {_id: 0}, uuid: t1Uuid},
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 3},
        ns: {db: "test", coll: "t1"},
        operationType: "replace",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing update of another field");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    assert.writeOK(db.t1.update({_id: 0}, {b: 3}));
    expected = {
        _id: {documentKey: {_id: 0}, uuid: t1Uuid},
        documentKey: {_id: 0},
        fullDocument: {_id: 0, b: 3},
        ns: {db: "test", coll: "t1"},
        operationType: "replace",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing upsert");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    assert.writeOK(db.t1.update({_id: 2}, {a: 4}, {upsert: true}));
    expected = {
        _id: {
            documentKey: {_id: 2},
            uuid: t1Uuid,
        },
        documentKey: {_id: 2},
        fullDocument: {_id: 2, a: 4},
        ns: {db: "test", coll: "t1"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing partial update with $inc");
    assert.writeOK(db.t1.insert({_id: 3, a: 5, b: 1}));
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    assert.writeOK(db.t1.update({_id: 3}, {$inc: {b: 2}}));
    expected = {
        _id: {documentKey: {_id: 3}, uuid: t1Uuid},
        documentKey: {_id: 3},
        ns: {db: "test", coll: "t1"},
        operationType: "update",
        updateDescription: {removedFields: [], updatedFields: {b: 3}},
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing delete");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    assert.writeOK(db.t1.remove({_id: 1}));
    expected = {
        _id: {documentKey: {_id: 1}, uuid: t1Uuid},
        documentKey: {_id: 1},
        ns: {db: "test", coll: "t1"},
        operationType: "delete",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing intervening write on another collection");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    let t2cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t2});
    assert.writeOK(db.t2.insert({_id: 100, c: 1}));
    const t2Uuid = getUUIDFromListCollections(db, db.t2.getName());
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: []});
    expected = {
        _id: {
            documentKey: {_id: 100},
            uuid: t2Uuid,
        },
        documentKey: {_id: 100},
        fullDocument: {_id: 100, c: 1},
        ns: {db: "test", coll: "t2"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: t2cursor, expectedChanges: [expected]});

    jsTestLog("Testing drop of unrelated collection");
    assert.writeOK(db.dropping.insert({}));
    db.dropping.drop();
    // Should still see the previous change from t2, shouldn't see anything about 'dropping'.

    jsTestLog("Testing rename");
    db.t3.drop();
    t2cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t2});
    assert.writeOK(db.t2.renameCollection("t3"));
    expected = {_id: {uuid: t2Uuid}, operationType: "invalidate"};
    cst.assertNextChangesEqual(
        {cursor: t2cursor, expectedChanges: [expected], expectInvalidate: true});

    jsTestLog("Testing insert that looks like rename");
    db.dne1.drop();
    db.dne2.drop();
    const dne1cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.dne1});
    const dne2cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.dne2});
    assert.writeOK(db.t3.insert({_id: 101, renameCollection: "test.dne1", to: "test.dne2"}));
    cst.assertNextChangesEqual({cursor: dne1cursor, expectedChanges: []});
    cst.assertNextChangesEqual({cursor: dne2cursor, expectedChanges: []});

    const isMongos = db.runCommand({isdbgrid: 1}).isdbgrid;
    if (!isMongos) {
        jsTestLog("Ensuring attempt to read with legacy operations fails.");
        db.getMongo().forceReadMode('legacy');
        const legacyCursor = db.tailable2.aggregate([{$changeStream: {}}, cst.oplogProjection],
                                                    {cursor: {batchSize: 0}});
        assert.throws(function() {
            legacyCursor.next();
        }, [], "Legacy getMore expected to fail on changeStream cursor.");
        db.getMongo().forceReadMode('commands');
    }

    jsTestLog("Testing resumability");
    db.resume1.drop();
    assert.commandWorked(db.createCollection("resume1"));

    // Note we do not project away 'id.ts' as it is part of the resume token.
    let resumeCursor = cst.startWatchingChanges(
        {pipeline: [{$changeStream: {}}], collection: db.resume1, includeTs: true});

    // Insert a document and save the resulting change stream.
    assert.writeOK(db.resume1.insert({_id: 1}));
    const firstInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(firstInsertChangeDoc.fullDocument, {_id: 1});

    jsTestLog("Testing resume after one document.");
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id}}],
        collection: db.resume1,
        includeTs: true,
        aggregateOptions: {cursor: {batchSize: 0}},
    });

    jsTestLog("Inserting additional documents.");
    assert.writeOK(db.resume1.insert({_id: 2}));
    const secondInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(secondInsertChangeDoc.fullDocument, {_id: 2});
    assert.writeOK(db.resume1.insert({_id: 3}));
    const thirdInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(thirdInsertChangeDoc.fullDocument, {_id: 3});

    jsTestLog("Testing resume after first document of three.");
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id}}],
        collection: db.resume1,
        includeTs: true,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    assert.docEq(cst.getOneChange(resumeCursor), secondInsertChangeDoc);
    assert.docEq(cst.getOneChange(resumeCursor), thirdInsertChangeDoc);

    jsTestLog("Testing resume after second document of three.");
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: secondInsertChangeDoc._id}}],
        collection: db.resume1,
        includeTs: true,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    assert.docEq(cst.getOneChange(resumeCursor), thirdInsertChangeDoc);

    cst.cleanUp();
}());
