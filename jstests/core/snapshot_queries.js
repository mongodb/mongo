// Regression test for edge cases in which .snapshot() queries could historically miss documents or
// return the same document twice.
(function() {
    'use strict';

    var cursor;
    var coll = db.snapshot_queries;
    coll.drop();

    //
    // Test that a large update to the document we just returned won't cause us to return it again.
    //

    for (var i = 0; i < 3; i++) {
        assert.writeOK(coll.insert({_id: i}));
    }

    cursor = coll.find().batchSize(2).snapshot();
    assert.eq(0, cursor.next()["_id"]);
    assert.eq(1, cursor.next()["_id"]);

    // Force a document move (on MMAP) while the query is yielded for a getMore.
    var bigString = Array(1024 * 1024).toString();
    assert.writeOK(coll.update({_id: 1}, {$set: {padding: bigString}}));

    assert.eq(2, cursor.next()["_id"]);
    assert(!cursor.hasNext());

    //
    // Test that a large update to the document we are about to return won't cause us to skip that
    // doc.
    //

    coll.drop();
    assert.writeOK(coll.insert({_id: 0, padding: Array(1000).toString()}));
    for (var i = 1; i <= 3; i++) {
        assert.writeOK(coll.insert({_id: i}));
    }

    // On MMAP, this will leave space at the beginning of the collection. A document can be moved
    // into this free space.
    assert.writeOK(coll.remove({_id: 0}));

    cursor = coll.find().snapshot().batchSize(2);
    assert.eq(1, cursor.next()["_id"]);
    assert.eq(2, cursor.next()["_id"]);

    // Force a document move (on MMAP) while the query is yielded for a getMore.
    assert.writeOK(coll.update({_id: 3}, {$set: {padding: Array(100).toString()}}));

    assert.eq(3, cursor.next()["_id"]);
    assert(!cursor.hasNext());
})();
