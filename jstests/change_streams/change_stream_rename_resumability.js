// Tests resuming on a change stream that was invalidated due to rename.

(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    let coll = assertDropAndRecreateCollection(db, "change_stream_invalidate_resumability");

    // Drop the collection we'll rename to _before_ starting the changeStream, so that we don't
    // get accidentally an invalidate when running on the whole DB or cluster.
    assertDropCollection(db, coll.getName() + "_renamed");

    const cursor = coll.watch();
    assert(!cursor.hasNext());

    // Create an 'insert' oplog entry.
    assert.writeOK(coll.insert({_id: 1}));

    assert.commandWorked(coll.renameCollection(coll.getName() + "_renamed"));

    // Update 'coll' to point to the renamed collection.
    coll = db[coll.getName() + "_renamed"];

    // Insert another document after the rename.
    assert.writeOK(coll.insert({_id: 2}));

    // We should get 2 oplog entries of type insert and invalidate.
    assert.soon(() => cursor.hasNext());
    let change = cursor.next();
    assert.eq(change.operationType, "insert", tojson(change));
    assert.docEq(change.fullDocument, {_id: 1});

    assert.soon(() => cursor.hasNext());
    change = cursor.next();
    assert.eq(change.operationType, "invalidate", tojson(change));
    assert(cursor.isExhausted());

    // TODO SERVER-34789: The code below should throw an error. We exercise this behavior here to
    // be sure that it doesn't crash the server, but the ability to resume a change stream after an
    // invalidate is a bug, not a feature.

    // Try resuming from the invalidate.
    assert.doesNotThrow(function() {
        const resumeCursor = coll.watch([], {resumeAfter: change._id});
        assert.soon(() => resumeCursor.hasNext());
        // Not checking the contents of the document returned, because we do not technically
        // support this behavior.
        resumeCursor.next();
    });
}());
