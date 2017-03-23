// Tests for invalidation during a getmore. This behavior is storage-engine dependent.
// See SERVER-16675.
(function() {
    "use strict";

    var t = db.jstests_getmore_invalidation;

    var count;
    var cursor;
    var nextDoc;
    var x;
    var y;

    // Case #1: Text search with deletion invalidation.
    t.drop();
    assert.commandWorked(t.ensureIndex({a: "text"}));
    assert.writeOK(t.insert({_id: 1, a: "bar"}));
    assert.writeOK(t.insert({_id: 2, a: "bar"}));
    assert.writeOK(t.insert({_id: 3, a: "bar"}));

    cursor = t.find({$text: {$search: "bar"}}).batchSize(2);
    cursor.next();
    cursor.next();

    assert.writeOK(t.remove({_id: 3}));

    // We should get back the document or not (depending on the storage engine / concurrency model).
    // Either is fine as long as we don't crash.
    count = cursor.itcount();
    assert(count === 0 || count === 1);

    // Case #2: Text search with mutation invalidation.
    t.drop();
    assert.commandWorked(t.ensureIndex({a: "text"}));
    assert.writeOK(t.insert({_id: 1, a: "bar"}));
    assert.writeOK(t.insert({_id: 2, a: "bar"}));
    assert.writeOK(t.insert({_id: 3, a: "bar"}));

    cursor = t.find({$text: {$search: "bar"}}).batchSize(2);
    cursor.next();
    cursor.next();

    // Update the next matching doc so that it no longer matches.
    assert.writeOK(t.update({_id: 3}, {$set: {a: "nomatch"}}));

    // Either the cursor should skip the result that no longer matches, or we should get back the
    // old
    // version of the doc.
    assert(!cursor.hasNext() || cursor.next()["a"] === "bar");

    // Case #3: Merge sort with deletion invalidation.
    t.drop();
    assert.commandWorked(t.ensureIndex({a: 1, b: 1}));
    assert.writeOK(t.insert({a: 1, b: 1}));
    assert.writeOK(t.insert({a: 1, b: 2}));
    assert.writeOK(t.insert({a: 2, b: 3}));
    assert.writeOK(t.insert({a: 2, b: 4}));

    cursor = t.find({a: {$in: [1, 2]}}).sort({b: 1}).batchSize(2);
    cursor.next();
    cursor.next();

    assert.writeOK(t.remove({a: 2, b: 3}));

    count = cursor.itcount();
    assert(count === 1 || count === 2);

    // Case #4: Merge sort with mutation invalidation.
    t.drop();
    assert.commandWorked(t.ensureIndex({a: 1, b: 1}));
    assert.writeOK(t.insert({a: 1, b: 1}));
    assert.writeOK(t.insert({a: 1, b: 2}));
    assert.writeOK(t.insert({a: 2, b: 3}));
    assert.writeOK(t.insert({a: 2, b: 4}));

    cursor = t.find({a: {$in: [1, 2]}}).sort({b: 1}).batchSize(2);
    cursor.next();
    cursor.next();

    assert.writeOK(t.update({a: 2, b: 3}, {$set: {a: 6}}));

    // Either the cursor should skip the result that no longer matches, or we should get back the
    // old
    // version of the doc.
    assert(cursor.hasNext());
    assert(cursor.next()["a"] === 2);
    if (cursor.hasNext()) {
        assert(cursor.next()["a"] === 2);
    }
    assert(!cursor.hasNext());

    // Case #5: 2d near with deletion invalidation.
    t.drop();
    t.ensureIndex({geo: "2d"});
    for (x = -1; x < 1; x++) {
        for (y = -1; y < 1; y++) {
            assert.writeOK(t.insert({geo: [x, y]}));
        }
    }

    cursor = t.find({geo: {$near: [0, 0], $maxDistance: 5}}).batchSize(2);
    cursor.next();
    cursor.next();

    // Drop all documents in the collection.
    assert.writeOK(t.remove({}));

    // Both MMAP v1 and doc-locking storage engines should force fetch the doc (it will be buffered
    // because it is the same distance from the center point as a doc already returned).
    assert(cursor.hasNext());

    // Case #6: 2dsphere near with deletion invalidation.
    t.drop();
    t.ensureIndex({geo: "2dsphere"});
    for (x = -1; x < 1; x++) {
        for (y = -1; y < 1; y++) {
            assert.writeOK(t.insert({geo: [x, y]}));
        }
    }

    cursor = t.find({geo: {$nearSphere: [0, 0], $maxDistance: 5}}).batchSize(2);
    cursor.next();
    cursor.next();

    // Drop all documents in the collection.
    assert.writeOK(t.remove({}));

    // Both MMAP v1 and doc-locking storage engines should force fetch the doc (it will be buffered
    // because it is the same distance from the center point as a doc already returned).
    assert(cursor.hasNext());

    // Case #7: 2dsphere near with deletion invalidation (again).
    t.drop();
    t.ensureIndex({geo: "2dsphere"});
    for (x = 0; x < 6; x++) {
        assert.writeOK(t.insert({geo: [x, x]}));
    }

    cursor = t.find({geo: {$nearSphere: [0, 0], $maxDistance: 10}}).batchSize(2);
    cursor.next();
    cursor.next();

    // Drop all documents in the collection.
    assert.writeOK(t.remove({}));

    // We might force-fetch or we might skip over the deleted documents, depending on the internals
    // of the geo near search. Just make sure that we can exhaust the cursor without crashing.
    assert.gte(cursor.itcount(), 0);

    // Case #8: 2d near with mutation invalidation.
    t.drop();
    t.ensureIndex({geo: "2d"});
    for (x = -1; x < 1; x++) {
        for (y = -1; y < 1; y++) {
            assert.writeOK(t.insert({geo: [x, y]}));
        }
    }

    cursor = t.find({geo: {$near: [0, 0], $maxDistance: 5}}).batchSize(2);
    cursor.next();
    cursor.next();

    // Update all documents in the collection to have position [15, 15].
    assert.writeOK(t.update({}, {$set: {geo: [15, 15]}}, false, true));

    // The old version of the document should be returned (the update should not be reflected in the
    // results of the near search).
    nextDoc = cursor.next();
    printjson(nextDoc);
    assert.neq([15, 15], nextDoc.geo);
    assert(nextDoc.geo[0] === 0 || nextDoc.geo[1] === 0);

    // Case #9: 2dsphere near with mutation invalidation.
    t.drop();
    t.ensureIndex({geo: "2dsphere"});
    for (x = -1; x < 1; x++) {
        for (y = -1; y < 1; y++) {
            assert.writeOK(t.insert({geo: [x, y]}));
        }
    }

    cursor = t.find({geo: {$nearSphere: [0, 0], $maxDistance: 5}}).batchSize(2);
    cursor.next();
    cursor.next();

    // Update all documents in the collection to have position [15, 15].
    assert.writeOK(t.update({}, {$set: {geo: [15, 15]}}, false, true));

    // The old version of the document should be returned (the update should not be reflected in the
    // results of the near search).
    nextDoc = cursor.next();
    printjson(nextDoc);
    assert.neq([15, 15], nextDoc.geo);
    assert(nextDoc.geo[0] === 0 || nextDoc.geo[1] === 0);

    // Case #10: sort with deletion invalidation.
    t.drop();
    t.ensureIndex({a: 1});
    t.insert({a: 1, b: 2});
    t.insert({a: 3, b: 3});
    t.insert({a: 2, b: 1});

    cursor = t.find({a: {$in: [1, 2, 3]}}).sort({b: 1}).batchSize(2);
    cursor.next();
    cursor.next();

    assert.writeOK(t.remove({a: 2}));

    if (cursor.hasNext()) {
        assert.eq(cursor.next().b, 3);
    }

    // Case #11: sort with mutation invalidation.
    t.drop();
    t.ensureIndex({a: 1});
    t.insert({a: 1, b: 2});
    t.insert({a: 3, b: 3});
    t.insert({a: 2, b: 1});

    cursor = t.find({a: {$in: [1, 2, 3]}}).sort({b: 1}).batchSize(2);
    cursor.next();
    cursor.next();

    assert.writeOK(t.update({a: 2}, {$set: {a: 4}}));

    count = cursor.itcount();
    if (cursor.hasNext()) {
        assert.eq(cursor.next().b, 3);
    }

})();
