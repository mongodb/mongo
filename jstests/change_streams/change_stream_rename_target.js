// Tests that watching a collection which another collection is renamed _to_ causes an invalidate.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    const testDB = db.getSiblingDB(jsTestName());

    // Write a document to the collection and test that the change stream returns it
    // and getMore command closes the cursor afterwards.
    const collName1 = "change_stream_rename_1";
    const collName2 = "change_stream_rename_2";
    let coll = assertDropAndRecreateCollection(testDB, collName1);
    assertDropCollection(testDB, collName2);
    assertDropCollection(testDB, collName2);

    // Watch the collection which doesn't exist yet.
    let aggCursor = testDB[collName2].watch();

    // Insert something to the collection we're _not_ watching.
    assert.writeOK(coll.insert({_id: 1}));

    assert.eq(aggCursor.hasNext(), false);

    // Now rename the collection TO the collection that's being watched. This should invalidate the
    // change stream.
    assert.commandWorked(coll.renameCollection(collName2));
    assert.soon(() => aggCursor.hasNext());
    let invalidate = aggCursor.next();
    assert.eq(invalidate.operationType, "invalidate");
    assert(aggCursor.isExhausted());

    // Do another insert.
    assert.writeOK(testDB[collName2].insert({_id: 2}));

    // TODO SERVER-34789: The code below should throw an error. We exercise this behavior here to
    // be sure that it doesn't crash the server, but the ability to resume a change stream after an
    // invalidate is a bug, not a feature.

    // Try resuming from the invalidate.
    assert.doesNotThrow(function() {
        let cursor = testDB[collName2].watch([], {resumeAfter: invalidate._id});
        assert.soon(() => cursor.hasNext());
        // Not checking the contents of the document returned, because we do not technically
        // support this behavior.
        cursor.next();
    });
}());
