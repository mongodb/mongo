// @tags: [requires_getmore]

// Tests for the behavior of tailable cursors when a collection is dropped or the cursor is
// otherwise invalidated.
(function() {
    "use strict";

    const collName = "tailable_cursor_invalidation";
    const coll = db[collName];
    coll.drop();

    // Test that you cannot open a tailable cursor on a non-existent collection.
    assert.eq(0, assert.commandWorked(db.runCommand({find: collName})).cursor.id);
    assert.eq(0, assert.commandWorked(db.runCommand({find: collName, tailable: true})).cursor.id);
    assert.eq(0,
              assert.commandWorked(db.runCommand({find: collName, tailable: true, awaitData: true}))
                  .cursor.id);
    const emptyBatchCursorId =
        assert
            .commandWorked(
                db.runCommand({find: collName, tailable: true, awaitData: true, batchSize: 0}))
            .cursor.id;
    const isMongos = db.adminCommand({isdbgrid: 1}).isdbgrid;
    if (isMongos) {
        // Mongos will let you establish a cursor with batch size 0 and return to you before it
        // realizes the shard's cursor is exhausted. The next getMore should return a 0 cursor id
        // though.
        assert.neq(emptyBatchCursorId, 0);
        assert.eq(
            0,
            assert.commandWorked(db.runCommand({getMore: emptyBatchCursorId, collection: collName}))
                .cursor.id);
    } else {
        // A mongod should know immediately that the collection doesn't exist, and return a 0 cursor
        // id.
        assert.eq(0, emptyBatchCursorId);
    }

    function dropAndRecreateColl() {
        coll.drop();
        assert.commandWorked(db.createCollection(collName, {capped: true, size: 1024}));
        const numDocs = 4;
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < numDocs; ++i) {
            bulk.insert({_id: i});
        }
        assert.writeOK(bulk.execute());
    }
    dropAndRecreateColl();

    /**
     * Runs a find command to establish a cursor. Asserts that the command worked and that the
     * cursor id is not 0, then returns the cursor id.
     */
    function openCursor({tailable, awaitData}) {
        const findRes = assert.commandWorked(
            db.runCommand({find: collName, tailable: tailable, awaitData: awaitData}));
        assert.neq(findRes.cursor.id, 0);
        assert.eq(findRes.cursor.ns, coll.getFullName());
        return findRes.cursor.id;
    }

    // Test that a cursor cannot be found if a collection is dropped between a find and a getMore.
    let cursorId = openCursor({tailable: true, awaitData: false});
    dropAndRecreateColl();
    assert.commandFailedWithCode(db.runCommand({getMore: cursorId, collection: collName}),
                                 ErrorCodes.CursorNotFound);
    cursorId = openCursor({tailable: true, awaitData: true});
    dropAndRecreateColl();
    assert.commandFailedWithCode(db.runCommand({getMore: cursorId, collection: collName}),
                                 ErrorCodes.CursorNotFound);
}());
