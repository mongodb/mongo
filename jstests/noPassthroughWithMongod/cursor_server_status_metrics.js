/**
 * Tests for serverStatus metrics.cursor stats.
 */
(function() {
    var coll = db[jsTest.name()];
    coll.drop();
    assert.writeOK(coll.insert({_id: 1}));
    assert.writeOK(coll.insert({_id: 2}));
    assert.writeOK(coll.insert({_id: 3}));

    assert.eq(3, coll.find().count());

    function getCurrentCursorsOpen() {
        return db.serverStatus().metrics.cursor.open.total;
    }

    function getCurrentCursorsPinned() {
        return db.serverStatus().metrics.cursor.open.pinned;
    }

    var initialTotalOpen = getCurrentCursorsOpen();

    // We expect no pinned cursors
    assert.eq(0, getCurrentCursorsPinned());

    // Total open cursors should not have changed after exhausting a cursor.
    assert.eq(3, coll.find().itcount());
    assert.eq(initialTotalOpen, getCurrentCursorsOpen());
    assert.eq(3, coll.find().batchSize(2).itcount());
    assert.eq(initialTotalOpen, getCurrentCursorsOpen());
    assert.eq(3, coll.find().batchSize(1).itcount());
    assert.eq(initialTotalOpen, getCurrentCursorsOpen());

    assert.eq(3, coll.aggregate([]).itcount());
    assert.eq(initialTotalOpen, getCurrentCursorsOpen());
    assert.eq(3, coll.aggregate([], {cursor: {batchSize: 2}}).itcount());
    assert.eq(initialTotalOpen, getCurrentCursorsOpen());
    assert.eq(3, coll.aggregate([], {cursor: {batchSize: 1}}).itcount());
    assert.eq(initialTotalOpen, getCurrentCursorsOpen());

    // Total pinned cursors should remain zero exhausting a cursor.
    assert.eq(3, coll.find().itcount());
    assert.eq(0, getCurrentCursorsPinned());
    assert.eq(3, coll.find().batchSize(2).itcount());
    assert.eq(0, getCurrentCursorsPinned());
    assert.eq(3, coll.find().batchSize(1).itcount());
    assert.eq(0, getCurrentCursorsPinned());

    assert.eq(3, coll.aggregate([]).itcount());
    assert.eq(0, getCurrentCursorsPinned());
    assert.eq(3, coll.aggregate([], {cursor: {batchSize: 2}}).itcount());
    assert.eq(0, getCurrentCursorsPinned());
    assert.eq(3, coll.aggregate([], {cursor: {batchSize: 1}}).itcount());
    assert.eq(0, getCurrentCursorsPinned());

    // This cursor should remain open on the server, but not pinned.
    var cursor = coll.find().batchSize(2);
    cursor.next();
    assert.eq(initialTotalOpen + 1, getCurrentCursorsOpen());
    assert.eq(0, getCurrentCursorsPinned());

    // Same should be true after pulling the second document out of the cursor, since we haven't
    // issued a getMore yet.
    cursor.next();
    assert.eq(initialTotalOpen + 1, getCurrentCursorsOpen());
    assert.eq(0, getCurrentCursorsPinned());

    // Cursor no longer reported as open after being exhausted.
    cursor.next();
    assert(!cursor.hasNext());
    assert.eq(initialTotalOpen, getCurrentCursorsOpen());
    assert.eq(0, getCurrentCursorsPinned());

    // Same behavior expected for an aggregation cursor.
    var cursor = coll.aggregate([], {cursor: {batchSize: 2}});
    cursor.next();
    assert.eq(initialTotalOpen + 1, getCurrentCursorsOpen());
    assert.eq(0, getCurrentCursorsPinned());
    cursor.next();
    assert.eq(initialTotalOpen + 1, getCurrentCursorsOpen());
    assert.eq(0, getCurrentCursorsPinned());
    cursor.next();
    assert(!cursor.hasNext());
    assert.eq(initialTotalOpen, getCurrentCursorsOpen());
    assert.eq(0, getCurrentCursorsPinned());
}());
