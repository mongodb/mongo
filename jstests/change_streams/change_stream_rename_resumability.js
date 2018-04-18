// Tests resuming on a change stream that was invalidated due to rename.

(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    const coll = assertDropAndRecreateCollection(db, "change_stream_invalidate_resumability");

    // Drop the collection we'll rename to _before_ starting the changeStream, so that we don't
    // get accidentally an invalidate when running on the whole DB or cluster.
    assertDropCollection(db, coll.getName() + "_renamed");

    const cursor = coll.watch();
    assert(!cursor.hasNext());

    // Create an 'insert' oplog entry.
    assert.writeOK(coll.insert({_id: 1}));

    assert.commandWorked(coll.renameCollection(coll.getName() + "_renamed"));

    // Insert another document after the rename.
    assert.commandWorked(coll.insert({_id: 2}));

    // We should get 2 oplog entries of type insert and invalidate.
    assert.soon(() => cursor.hasNext());
    let change = cursor.next();
    assert.eq(change.operationType, "insert", tojson(change));
    assert.docEq(change.fullDocument, {_id: 1});

    assert.soon(() => cursor.hasNext());
    change = cursor.next();
    assert.eq(change.operationType, "invalidate", tojson(change));
    assert(cursor.isExhausted());

    // Try resuming from the invalidate.
    const resumeCursor = coll.watch([], {resumeAfter: change._id});

    // Be sure we can see the change after the rename.
    assert.soon(() => resumeCursor.hasNext());
    change = resumeCursor.next();
    assert.eq(change.operationType, "insert", tojson(change));
    assert.docEq(change.fullDocument, {_id: 2});
}());
