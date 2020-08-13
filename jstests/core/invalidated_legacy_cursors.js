/**
 * Test that all DBClientCursor cursor types throw an exception when the server returns
 * CursorNotFound.
 * @tags: [
 *   assumes_balancer_off,
 *   requires_getmore,
 *   requires_non_retryable_commands,
 *   sbe_incompatible,
 * ]
 */
(function() {
'use strict';

const testDB = db.getSiblingDB("invalidated_legacy_cursors");
const coll = testDB.test;
const nDocs = 10;
const batchSize = 2;  // The minimum DBClientCursor batch size is 2.

function setupCollection(isCapped) {
    coll.drop();
    if (isCapped) {
        assert.commandWorked(testDB.createCollection(coll.getName(), {capped: true, size: 4096}));
    }
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < nDocs; ++i) {
        bulk.insert({_id: i, x: i});
    }
    assert.commandWorked(bulk.execute());
    assert.commandWorked(coll.createIndex({x: 1}));
}

function testLegacyCursorThrowsCursorNotFound(isTailable) {
    coll.getMongo().forceReadMode("legacy");
    setupCollection(isTailable);

    // Create a cursor and consume the docs in the first batch.
    let cursor = coll.find().batchSize(batchSize);
    if (isTailable) {
        cursor = cursor.tailable();
    }
    for (let i = 0; i < batchSize; i++) {
        cursor.next();
    }

    // Kill the cursor and assert that the cursor throws CursorNotFound on the first next() call.
    // Use killCursors instead of cursor.close() since we still want to send getMore requests
    // through the existing cursor.
    assert.commandWorked(
        testDB.runCommand({killCursors: coll.getName(), cursors: [cursor.getId()]}));
    const error = assert.throws(() => cursor.next());
    assert.eq(error.code, ErrorCodes.CursorNotFound);

    // Check the state of the cursor.
    assert(!cursor.hasNext());
    assert.eq(0, cursor.getId());
    assert.throws(() => cursor.next());
}

testLegacyCursorThrowsCursorNotFound(false);
if (!jsTest.options().mixedBinVersions) {
    testLegacyCursorThrowsCursorNotFound(true);
}
}());
