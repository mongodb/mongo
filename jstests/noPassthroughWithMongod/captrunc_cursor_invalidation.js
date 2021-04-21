// Test that when a capped collection is truncated, tailable cursors die on getMore with the error
// code 'CappedPositionLost'.
//
// @tags: [requires_capped]
(function() {
"use strict";

const coll = db.captrunc_cursor_invalidation;
coll.drop();

// Create a capped collection with four documents.
assert.commandWorked(db.createCollection(coll.getName(), {capped: true, size: 1024}));
const numDocs = 4;
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; ++i) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

// Open a tailable cursor against the capped collection.
const findRes = assert.commandWorked(db.runCommand({find: coll.getName(), tailable: true}));
assert.neq(findRes.cursor.id, 0);
assert.eq(findRes.cursor.ns, coll.getFullName());
assert.eq(findRes.cursor.firstBatch.length, 4);
const cursorId = findRes.cursor.id;

// Truncate the capped collection so that the cursor's position no longer exists.
assert.commandWorked(db.runCommand({captrunc: coll.getName(), n: 2}));

// A subsequent getMore should fail with 'CappedPositionLost'.
assert.commandFailedWithCode(db.runCommand({getMore: cursorId, collection: coll.getName()}),
                             ErrorCodes.CappedPositionLost);

// The cursor has now been destroyed, so another getMore should fail with 'CursorNotFound'.
assert.commandFailedWithCode(db.runCommand({getMore: cursorId, collection: coll.getName()}),
                             ErrorCodes.CursorNotFound);
}());
