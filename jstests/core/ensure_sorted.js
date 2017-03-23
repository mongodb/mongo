// SERVER-17011 Tests whether queries which specify sort and batch size can generate results out of
// order due to the ntoreturn hack. The EnsureSortedStage should solve this problem.
(function() {
    'use strict';
    var coll = db.ensure_sorted;

    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    assert.writeOK(coll.insert({a: 1, b: 4}));
    assert.writeOK(coll.insert({a: 2, b: 3}));
    assert.writeOK(coll.insert({a: 3, b: 2}));
    assert.writeOK(coll.insert({a: 4, b: 1}));

    var cursor = coll.find({a: {$lt: 5}}).sort({b: -1}).batchSize(2);
    cursor.next();  // {a: 1, b: 4}.
    cursor.next();  // {a: 2, b: 3}.

    assert.writeOK(coll.update({b: 2}, {$set: {b: 5}}));
    var result = cursor.next();

    // We might either drop the document where "b" is 2 from the result set, or we might include the
    // old version of this document (before the update is applied). Either is acceptable, but
    // out-of-order results are unacceptable.
    assert(result.b === 2 || result.b === 1, "cursor returned: " + printjson(result));
})();
