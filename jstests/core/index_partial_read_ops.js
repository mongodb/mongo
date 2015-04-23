// Read ops tests for partial indexes.

(function() {
    "use strict";
    var ret;
    var coll = db.index_partial_read_ops;
    coll.drop();

    coll.ensureIndex({x: 1}, {partialFilterExpression: {a: 1}});
    assert.writeOK(coll.insert({x: 5, a: 2})); // Not in index.
    assert.writeOK(coll.insert({x: 6, a: 1})); // In index.

    // Verify index counts and basic functionality.
    assert.eq(1, coll.find({x: 6}).itcount());
    assert.eq(1, coll.find({x: 6, a: 1}).hint({x: 1}).itcount());
    assert.eq(1, coll.find({x: {$gt: 1}, a: 1}).hint({x: 1}).itcount());
    // Hint is bad because a < 5 is not a subset of a = 1
    assert.throws(function() {
                  coll.find({x: 6, a: {$lt: 5}}).hint({x: 1}).itcount()
                  });

    // Count has special query path
    assert.eq(1, coll.find({x: {$gt: 1}, a: 1}).hint({x: 1}).count());

})();
