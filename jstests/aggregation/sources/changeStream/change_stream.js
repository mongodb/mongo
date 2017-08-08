// Basic $changeStream tests.
(function() {
    "use strict";

    const oplogProjection = {$project: {"_id.clusterTime": 0}};
    function getCollectionNameFromFullNamespace(ns) {
        return ns.split(/\.(.+)/)[1];
    }

    // Helpers for testing that pipeline returns correct set of results.  Run startWatchingChanges
    // with the pipeline, then insert the changes, then run assertNextBatchMatches with the result
    // of startWatchingChanges and the expected set of results.
    function startWatchingChanges(pipeline, collection) {
        // Strip the oplog fields we aren't testing.
        pipeline.push(oplogProjection);
        // Waiting for replication assures no previous operations will be included.
        replTest.awaitReplication();
        let res = assert.commandWorked(
            db.runCommand({aggregate: collection.getName(), "pipeline": pipeline, cursor: {}}));
        assert.neq(res.cursor.id, 0);
        return res.cursor;
    }

    function assertNextBatchMatches({cursor, expectedBatch}) {
        replTest.awaitReplication();
        if (expectedBatch.length == 0)
            assert.commandWorked(db.adminCommand(
                {configureFailPoint: "disableAwaitDataForGetMoreCmd", mode: "alwaysOn"}));
        let res = assert.commandWorked(db.runCommand({
            getMore: cursor.id,
            collection: getCollectionNameFromFullNamespace(cursor.ns),
            maxTimeMS: 5 * 60 * 1000,
            batchSize: (expectedBatch.length + 1)
        }));
        if (expectedBatch.length == 0)
            assert.commandWorked(db.adminCommand(
                {configureFailPoint: "disableAwaitDataForGetMoreCmd", mode: "off"}));
        assert.docEq(res.cursor.nextBatch, expectedBatch);
    }

    let replTest = new ReplSetTest({name: 'changeStreamTest', nodes: 1});
    let nodes = replTest.startSet();
    replTest.initiate();
    replTest.awaitReplication();

    db = replTest.getPrimary().getDB('test');
    db.getMongo().forceReadMode('commands');

    jsTestLog("Testing single insert");
    let cursor = startWatchingChanges([{$changeStream: {}}], db.t1);
    assert.writeOK(db.t1.insert({_id: 0, a: 1}));
    let expected = {
        _id: {
            documentKey: {_id: 0},
            ns: "test.t1",
        },
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 1},
        ns: {db: "test", coll: "t1"},
        operationType: "insert",
    };
    assertNextBatchMatches({cursor: cursor, expectedBatch: [expected]});

    jsTestLog("Testing second insert");
    cursor = startWatchingChanges([{$changeStream: {}}], db.t1);
    assert.writeOK(db.t1.insert({_id: 1, a: 2}));
    expected = {
        _id: {
            documentKey: {_id: 1},
            ns: "test.t1",
        },
        documentKey: {_id: 1},
        fullDocument: {_id: 1, a: 2},
        ns: {db: "test", coll: "t1"},
        operationType: "insert",
    };
    assertNextBatchMatches({cursor: cursor, expectedBatch: [expected]});

    jsTestLog("Testing update");
    cursor = startWatchingChanges([{$changeStream: {}}], db.t1);
    assert.writeOK(db.t1.update({_id: 0}, {a: 3}));
    expected = {
        _id: {documentKey: {_id: 0}, ns: "test.t1"},
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 3},
        ns: {db: "test", coll: "t1"},
        operationType: "replace",
    };
    assertNextBatchMatches({cursor: cursor, expectedBatch: [expected]});

    jsTestLog("Testing update of another field");
    cursor = startWatchingChanges([{$changeStream: {}}], db.t1);
    assert.writeOK(db.t1.update({_id: 0}, {b: 3}));
    expected = {
        _id: {documentKey: {_id: 0}, ns: "test.t1"},
        documentKey: {_id: 0},
        fullDocument: {_id: 0, b: 3},
        ns: {db: "test", coll: "t1"},
        operationType: "replace",
    };
    assertNextBatchMatches({cursor: cursor, expectedBatch: [expected]});

    jsTestLog("Testing upsert");
    cursor = startWatchingChanges([{$changeStream: {}}], db.t1);
    assert.writeOK(db.t1.update({_id: 2}, {a: 4}, {upsert: true}));
    expected = {
        _id: {
            documentKey: {_id: 2},
            ns: "test.t1",
        },
        documentKey: {_id: 2},
        fullDocument: {_id: 2, a: 4},
        ns: {db: "test", coll: "t1"},
        operationType: "insert",
    };
    assertNextBatchMatches({cursor: cursor, expectedBatch: [expected]});

    jsTestLog("Testing partial update with $inc");
    assert.writeOK(db.t1.insert({_id: 3, a: 5, b: 1}));
    cursor = startWatchingChanges([{$changeStream: {}}], db.t1);
    assert.writeOK(db.t1.update({_id: 3}, {$inc: {b: 2}}));
    expected = {
        _id: {documentKey: {_id: 3}, ns: "test.t1"},
        documentKey: {_id: 3},
        fullDocument: null,
        ns: {db: "test", coll: "t1"},
        operationType: "update",
        updateDescription: {removedFields: [], updatedFields: {b: 3}},
    };
    assertNextBatchMatches({cursor: cursor, expectedBatch: [expected]});

    jsTestLog("Testing delete");
    cursor = startWatchingChanges([{$changeStream: {}}], db.t1);
    assert.writeOK(db.t1.remove({_id: 1}));
    expected = {
        _id: {documentKey: {_id: 1}, ns: "test.t1"},
        documentKey: {_id: 1},
        fullDocument: null,
        ns: {db: "test", coll: "t1"},
        operationType: "delete",
    };
    assertNextBatchMatches({cursor: cursor, expectedBatch: [expected]});

    jsTestLog("Testing intervening write on another collection");
    cursor = startWatchingChanges([{$changeStream: {}}], db.t1);
    let t2cursor = startWatchingChanges([{$changeStream: {}}], db.t2);
    assert.writeOK(db.t2.insert({_id: 100, c: 1}));
    assertNextBatchMatches({cursor: cursor, expectedBatch: []});
    expected = {
        _id: {
            documentKey: {_id: 100},
            ns: "test.t2",
        },
        documentKey: {_id: 100},
        fullDocument: {_id: 100, c: 1},
        ns: {db: "test", coll: "t2"},
        operationType: "insert",
    };
    assertNextBatchMatches({cursor: t2cursor, expectedBatch: [expected]});

    jsTestLog("Testing drop of unrelated collection");
    assert.writeOK(db.dropping.insert({}));
    db.dropping.drop();
    // Should still see the previous change from t2, shouldn't see anything about 'dropping'.

    jsTestLog("Testing rename");
    t2cursor = startWatchingChanges([{$changeStream: {}}], db.t2);
    assert.writeOK(db.t2.renameCollection("t3"));
    expected = {_id: {ns: "test.$cmd"}, operationType: "invalidate", fullDocument: null};
    assertNextBatchMatches({cursor: t2cursor, expectedBatch: [expected]});

    jsTestLog("Testing insert that looks like rename");
    const dne1cursor = startWatchingChanges([{$changeStream: {}}], db.dne1);
    const dne2cursor = startWatchingChanges([{$changeStream: {}}], db.dne2);
    assert.writeOK(db.t3.insert({_id: 101, renameCollection: "test.dne1", to: "test.dne2"}));
    assertNextBatchMatches({cursor: dne1cursor, expectedBatch: []});
    assertNextBatchMatches({cursor: dne2cursor, expectedBatch: []});

    // Now make sure the cursor behaves like a tailable awaitData cursor.
    jsTestLog("Testing tailability");
    const tailableCursor = db.tailable1.aggregate([{$changeStream: {}}, oplogProjection]);
    assert(!tailableCursor.hasNext());
    assert.writeOK(db.tailable1.insert({_id: 101, a: 1}));
    assert(tailableCursor.hasNext());
    assert.docEq(tailableCursor.next(), {
        _id: {
            documentKey: {_id: 101},
            ns: "test.tailable1",
        },
        documentKey: {_id: 101},
        fullDocument: {_id: 101, a: 1},
        ns: {db: "test", coll: "tailable1"},
        operationType: "insert",
    });

    jsTestLog("Testing awaitdata");
    let res = assert.commandWorked(db.runCommand(
        {aggregate: "tailable2", pipeline: [{$changeStream: {}}, oplogProjection], cursor: {}}));
    let aggcursor = res.cursor;

    // We should get a valid cursor.
    assert.neq(aggcursor.id, 0);

    // Initial batch size should be zero as there should be no data.
    assert.eq(aggcursor.firstBatch.length, 0);

    // No data, so should return no results, but cursor should remain valid.  Note we are
    // specifically testing awaitdata behavior here, so we cannot use the failpoint to skip the
    // wait.
    res = assert.commandWorked(
        db.runCommand({getMore: aggcursor.id, collection: "tailable2", maxTimeMS: 1000}));
    aggcursor = res.cursor;
    assert.neq(aggcursor.id, 0);
    assert.eq(aggcursor.nextBatch.length, 0);

    // Now insert something in parallel while waiting for it.
    let insertshell = startParallelShell(function() {
        // Wait for the getMore to appear in currentop.
        assert.soon(function() {
            return db.currentOp({op: "getmore", "command.collection": "tailable2"}).inprog.length ==
                1;
        });
        assert.writeOK(db.tailable2.insert({_id: 102, a: 2}));
    });
    res = assert.commandWorked(
        db.runCommand({getMore: aggcursor.id, collection: "tailable2", maxTimeMS: 5 * 60 * 1000}));
    aggcursor = res.cursor;
    assert.eq(aggcursor.nextBatch.length, 1);
    assert.docEq(aggcursor.nextBatch[0], {
        _id: {
            documentKey: {_id: 102},
            ns: "test.tailable2",
        },
        documentKey: {_id: 102},
        fullDocument: {_id: 102, a: 2},
        ns: {db: "test", coll: "tailable2"},
        operationType: "insert",
    });

    // Wait for insert shell to terminate.
    insertshell();

    jsTestLog("Ensuring attempt to read with legacy operations fails.");
    db.getMongo().forceReadMode('legacy');
    const legacyCursor =
        db.tailable2.aggregate([{$changeStream: {}}, oplogProjection], {cursor: {batchSize: 0}});
    assert.throws(function() {
        legacyCursor.next();
    }, [], "Legacy getMore expected to fail on changeStream cursor.");

    /**
     * Gets one document from the cursor using getMore with awaitData disabled. Asserts if no
     * document is present.
     */
    function getOneDoc(cursor) {
        replTest.awaitReplication();
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "disableAwaitDataForGetMoreCmd", mode: "alwaysOn"}));
        let res = assert.commandWorked(db.runCommand({
            getMore: cursor.id,
            collection: getCollectionNameFromFullNamespace(cursor.ns),
            batchSize: 1
        }));
        assert.eq(res.cursor.nextBatch.length, 1);
        assert.commandWorked(
            db.adminCommand({configureFailPoint: "disableAwaitDataForGetMoreCmd", mode: "off"}));
        return res.cursor.nextBatch[0];
    }

    /**
     * Attempts to get a document from the cursor with awaitData disabled, and asserts if a document
     * is present.
     */
    function assertNextBatchIsEmpty(cursor) {
        replTest.awaitReplication();
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "disableAwaitDataForGetMoreCmd", mode: "alwaysOn"}));
        let res = assert.commandWorked(db.runCommand({
            getMore: cursor.id,
            collection: getCollectionNameFromFullNamespace(cursor.ns),
            batchSize: 1
        }));
        assert.eq(res.cursor.nextBatch.length, 0);
        assert.commandWorked(
            db.adminCommand({configureFailPoint: "disableAwaitDataForGetMoreCmd", mode: "off"}));
    }

    jsTestLog("Testing resumability");
    assert.commandWorked(db.createCollection("resume1"));

    // Note we do not project away 'id.ts' as it is part of the resume token.
    res = assert.commandWorked(
        db.runCommand({aggregate: "resume1", pipeline: [{$changeStream: {}}], cursor: {}}));
    let resumeCursor = res.cursor;
    assert.neq(resumeCursor.id, 0);
    assert.eq(resumeCursor.firstBatch.length, 0);

    // Insert a document and save the resulting change stream.
    assert.writeOK(db.resume1.insert({_id: 1}));
    const firstInsertChangeDoc = getOneDoc(resumeCursor);
    assert.docEq(firstInsertChangeDoc.fullDocument, {_id: 1});

    jsTestLog("Testing resume after one document.");
    res = assert.commandWorked(db.runCommand({
        aggregate: "resume1",
        pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id}}],
        cursor: {batchSize: 0}
    }));
    resumeCursor = res.cursor;
    assertNextBatchIsEmpty(resumeCursor);

    jsTestLog("Inserting additional documents.");
    assert.writeOK(db.resume1.insert({_id: 2}));
    const secondInsertChangeDoc = getOneDoc(resumeCursor);
    assert.docEq(secondInsertChangeDoc.fullDocument, {_id: 2});
    assert.writeOK(db.resume1.insert({_id: 3}));
    const thirdInsertChangeDoc = getOneDoc(resumeCursor);
    assert.docEq(thirdInsertChangeDoc.fullDocument, {_id: 3});
    assertNextBatchIsEmpty(resumeCursor);

    jsTestLog("Testing resume after first document of three.");
    res = assert.commandWorked(db.runCommand({
        aggregate: "resume1",
        pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id}}],
        cursor: {batchSize: 0}
    }));
    resumeCursor = res.cursor;
    assert.docEq(getOneDoc(resumeCursor), secondInsertChangeDoc);
    assert.docEq(getOneDoc(resumeCursor), thirdInsertChangeDoc);
    assertNextBatchIsEmpty(resumeCursor);

    jsTestLog("Testing resume after second document of three.");
    res = assert.commandWorked(db.runCommand({
        aggregate: "resume1",
        pipeline: [{$changeStream: {resumeAfter: secondInsertChangeDoc._id}}],
        cursor: {batchSize: 0}
    }));
    resumeCursor = res.cursor;
    assert.docEq(getOneDoc(resumeCursor), thirdInsertChangeDoc);
    assertNextBatchIsEmpty(resumeCursor);

    jsTestLog("Testing that resume is possible after the collection is dropped.");
    assert(db.resume1.drop());
    const invalidateDoc = getOneDoc(resumeCursor);
    assert.eq(invalidateDoc.operationType, "invalidate");
    res = assert.commandWorked(db.runCommand({
        aggregate: "resume1",
        pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id}}],
        cursor: {batchSize: 0}
    }));
    resumeCursor = res.cursor;
    assert.docEq(getOneDoc(resumeCursor), secondInsertChangeDoc);
    assert.docEq(getOneDoc(resumeCursor), thirdInsertChangeDoc);
    assert.docEq(getOneDoc(resumeCursor), invalidateDoc);
    assertNextBatchIsEmpty(resumeCursor);

    replTest.stopSet();
}());
