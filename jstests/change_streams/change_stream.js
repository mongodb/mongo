// Basic $changeStream tests.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");
    load('jstests/libs/uuid_util.js');

    jsTestLog("Testing $changeStream on non-existent database");
    const dbDoesNotExist = db.getSiblingDB("database-does-not-exist");
    assert.commandWorked(dbDoesNotExist.dropDatabase());
    assert.commandFailedWithCode(
        dbDoesNotExist.runCommand(
            {aggregate: dbDoesNotExist.getName(), pipeline: [{$changeStream: {}}], cursor: {}}),
        ErrorCodes.NamespaceNotFound);

    let cst = new ChangeStreamTest(db);

    jsTestLog("Testing single insert");
    assertDropAndRecreateCollection(db, "t1");
    let cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    // Test that if there are no changes, we return an empty batch.
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    assert.writeOK(db.t1.insert({_id: 0, a: 1}));
    const t1Uuid = getUUIDFromListCollections(db, db.t1.getName());
    let expected = {
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
        documentKey: {_id: 1},
        fullDocument: {_id: 1, a: 2},
        ns: {db: "test", coll: "t1"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing update");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    assert.writeOK(db.t1.update({_id: 0}, {_id: 0, a: 3}));
    expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 3},
        ns: {db: "test", coll: "t1"},
        operationType: "replace",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing update of another field");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    assert.writeOK(db.t1.update({_id: 0}, {_id: 0, b: 3}));
    expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, b: 3},
        ns: {db: "test", coll: "t1"},
        operationType: "replace",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing upsert");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    assert.writeOK(db.t1.update({_id: 2}, {_id: 2, a: 4}, {upsert: true}));
    expected = {
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
        documentKey: {_id: 1},
        ns: {db: "test", coll: "t1"},
        operationType: "delete",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing intervening write on another collection");
    assertDropCollection(db, "t2");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    let t2cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t2});
    assert.writeOK(db.t2.insert({_id: 100, c: 1}));
    const t2Uuid = getUUIDFromListCollections(db, db.t2.getName());
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: []});
    expected = {
        documentKey: {_id: 100},
        fullDocument: {_id: 100, c: 1},
        ns: {db: "test", coll: "t2"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: t2cursor, expectedChanges: [expected]});

    jsTestLog("Testing drop of unrelated collection");
    assert.writeOK(db.dropping.insert({}));
    assertDropCollection(db, db.dropping.getName());
    // Should still see the previous change from t2, shouldn't see anything about 'dropping'.

    // Test collection renaming. Sharded collections cannot be renamed.
    if (!db.t2.stats().sharded) {
        jsTestLog("Testing rename");
        assertDropCollection(db, "t3");
        t2cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t2});
        assert.writeOK(db.t2.renameCollection("t3"));
        expected = {operationType: "invalidate"};
        cst.assertNextChangesEqual(
            {cursor: t2cursor, expectedChanges: [expected], expectInvalidate: true});
    }

    jsTestLog("Testing insert that looks like rename");
    assertDropCollection(db, "dne1");
    assertDropCollection(db, "dne2");
    const dne1cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.dne1});
    const dne2cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.dne2});
    assert.writeOK(db.t2.insert({_id: 101, renameCollection: "test.dne1", to: "test.dne2"}));
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

    cst.cleanUp();
}());
