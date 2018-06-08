// @tags: [requires_getmore, requires_capped]

// Tests for the behavior of combining the tailable and awaitData options to the getMore command
// with the batchSize option.
(function() {
    "use strict";

    const collName = "tailable_getmore_batch_size";
    const coll = db[collName];
    const batchSize = 2;

    function dropAndRecreateColl({numDocs}) {
        coll.drop();
        assert.commandWorked(db.createCollection(collName, {capped: true, size: 1024}));
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < numDocs; ++i) {
            bulk.insert({_id: i});
        }
        assert.writeOK(bulk.execute());
    }

    // Test that running a find with the 'tailable' option will return results immediately, even if
    // there are fewer than the specified batch size.
    dropAndRecreateColl({numDocs: batchSize - 1});
    let findRes =
        assert.commandWorked(db.runCommand({find: collName, tailable: true, batchSize: batchSize}));
    assert.eq(findRes.cursor.firstBatch.length, batchSize - 1);
    assert.neq(findRes.cursor.id, 0);
    // Test that the same is true for a find with the 'tailable' and 'awaitData' options set.
    findRes = assert.commandWorked(
        db.runCommand({find: collName, tailable: true, awaitData: true, batchSize: batchSize}));
    assert.eq(findRes.cursor.firstBatch.length, batchSize - 1);
    assert.neq(findRes.cursor.id, 0);

    /**
     * Runs a find command with a batchSize of 'batchSize' to establish a cursor. Asserts that the
     * command worked and that the cursor id is not 0, then returns the cursor id.
     */
    function openCursor({batchSize, tailable, awaitData}) {
        const findRes = assert.commandWorked(db.runCommand(
            {find: collName, tailable: tailable, awaitData: awaitData, batchSize: batchSize}));
        assert.eq(findRes.cursor.firstBatch.length, batchSize);
        assert.neq(findRes.cursor.id, 0);
        assert.eq(findRes.cursor.ns, coll.getFullName());
        return findRes.cursor.id;
    }

    // Test that specifying a batch size to a getMore on a tailable cursor produces a batch of the
    // desired size when the number of results is larger than the batch size.

    // One batch's worth for the find and one more than one batch's worth for the getMore.
    dropAndRecreateColl({numDocs: batchSize + (batchSize + 1)});
    let cursorId = openCursor({batchSize: batchSize, tailable: true, awaitData: false});
    let getMoreRes = assert.commandWorked(
        db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}));
    assert.eq(getMoreRes.cursor.nextBatch.length, batchSize);

    // Test that the same is true for a tailable, *awaitData* cursor.
    cursorId = openCursor({batchSize: batchSize, tailable: true, awaitData: true});
    getMoreRes = assert.commandWorked(
        db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}));
    assert.eq(getMoreRes.cursor.nextBatch.length, batchSize);

    // Test that specifying a batch size to a getMore on a tailable cursor returns all
    // new results immediately, even if the batch size is larger than the number of new results.
    // One batch's worth for the find and one less than one batch's worth for the getMore.
    dropAndRecreateColl({numDocs: batchSize + (batchSize - 1)});
    cursorId = openCursor({batchSize: batchSize, tailable: true, awaitData: false});
    getMoreRes = assert.commandWorked(
        db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}));
    assert.eq(getMoreRes.cursor.nextBatch.length, batchSize - 1);

    // Test that the same is true for a tailable, *awaitData* cursor.
    cursorId = openCursor({batchSize: batchSize, tailable: true, awaitData: true});
    getMoreRes = assert.commandWorked(
        db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}));
    assert.eq(getMoreRes.cursor.nextBatch.length, batchSize - 1);

    // Test that using a smaller batch size than there are results will return all results without
    // empty batches in between (SERVER-30799).
    dropAndRecreateColl({numDocs: batchSize * 3});
    cursorId = openCursor({batchSize: batchSize, tailable: true, awaitData: false});
    getMoreRes = assert.commandWorked(
        db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}));
    assert.eq(getMoreRes.cursor.nextBatch.length, batchSize);
    getMoreRes = assert.commandWorked(
        db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}));
    assert.eq(getMoreRes.cursor.nextBatch.length, batchSize);
    getMoreRes = assert.commandWorked(
        db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}));
    assert.eq(getMoreRes.cursor.nextBatch.length, 0);

    // Avoid leaving the cursor open. Cursors above are killed by drops, but we'll avoid dropping
    // the collection at the end so other consistency checks like validate can be run against it.
    assert.commandWorked(db.runCommand({killCursors: collName, cursors: [getMoreRes.cursor.id]}));
}());
