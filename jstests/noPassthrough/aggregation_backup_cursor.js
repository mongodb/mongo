/**
 * Test the basic operation of a `$backupCursor` aggregation stage.
 *
 * @tags: [requires_persistence, requires_wiredtiger]
 */
(function() {
    "use strict";

    let conn = MongoRunner.runMongod();
    let db = conn.getDB("test");

    let backupCursor = db.aggregate([{$backupCursor: {}}]);
    // There should be about 14 files in total, but being precise would be unnecessarily fragile.
    assert.gt(backupCursor.itcount(), 6);
    assert(!backupCursor.isExhausted());
    backupCursor.close();

    // Open a backup cursor. Use a small batch size to ensure a getMore retrieves additional
    // results.
    let response = assert.commandWorked(
        db.runCommand({aggregate: 1, pipeline: [{$backupCursor: {}}], cursor: {batchSize: 2}}));
    assert.eq("test.$cmd.aggregate", response.cursor.ns);
    assert.eq(2, response.cursor.firstBatch.length);
    let cursorId = response.cursor.id;

    response =
        assert.commandWorked(db.runCommand({getMore: cursorId, collection: "$cmd.aggregate"}));
    // Sanity check the results.
    assert.neq(0, response.cursor.id);
    assert.gt(response.cursor.nextBatch.length, 6);

    // The $backupCursor is a tailable cursor. Even though we've exhausted the results, running a
    // getMore should succeed.
    response =
        assert.commandWorked(db.runCommand({getMore: cursorId, collection: "$cmd.aggregate"}));
    assert.neq(0, response.cursor.id);
    assert.eq(0, response.cursor.nextBatch.length);

    // Because the backup cursor is still open, trying to open a second cursor should fail.
    assert.commandFailed(
        db.runCommand({aggregate: 1, pipeline: [{$backupCursor: {}}], cursor: {}}));

    // Kill the backup cursor.
    response =
        assert.commandWorked(db.runCommand({killCursors: "$cmd.aggregate", cursors: [cursorId]}));
    assert.eq(1, response.cursorsKilled.length);
    assert.eq(cursorId, response.cursorsKilled[0]);

    // Open another backup cursor with a batch size of 0. The underlying backup cursor should be
    // created.
    response = assert.commandWorked(
        db.runCommand({aggregate: 1, pipeline: [{$backupCursor: {}}], cursor: {batchSize: 0}}));
    assert.neq(0, response.cursor.id);
    assert.eq(0, response.cursor.firstBatch.length);

    // Attempt to open a second backup cursor to demonstrate the original underlying cursor was
    // opened.
    assert.commandFailed(
        db.runCommand({aggregate: 1, pipeline: [{$backupCursor: {}}], cursor: {}}));

    // Demonstrate query cursor timeouts will kill backup cursors, closing the underlying resources.
    assert.commandWorked(db.adminCommand({setParameter: 1, cursorTimeoutMillis: 1}));
    assert.soon(() => {
        return db.runCommand({aggregate: 1, pipeline: [{$backupCursor: {}}], cursor: {}})['ok'] ==
            1;
    });

    MongoRunner.stopMongod(conn);
})();
