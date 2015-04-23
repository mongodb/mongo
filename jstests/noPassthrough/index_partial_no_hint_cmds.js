// Test partial indexes with commands that won't test well
// under sharding passthrough
(function () {
    "use strict";
    // Launch mongod with notable scan, since these tests are
    // centered around operations that can't take a hint.
    var runner = MongoRunner.runMongod({setParameter: "notablescan=1"});
    var coll = runner.getDB("test").index_partial_no_hint_cmds;
    var ret;

    var getNumKeys = function(idxName) {
        var res = assert.commandWorked(coll.validate(true));
        return res.keysPerIndex[coll.getFullName() + ".$" + idxName];
    };

    coll.drop();

    assert.commandWorked(coll.ensureIndex({x: 1}, {partialFilterExpression: {a: 1}}));

    assert.writeOK(coll.insert({_id: 1, x: 5, a: 2})); // Not in index.
    assert.writeOK(coll.insert({_id: 2, x: 6, a: 1})); // In index.

    // Verify we will throw if the partial index isn't used. Find won't
    // necessarily assert in this case, but findAndModify does consistenly.
    assert.throws(function() {
                  coll.findAndModify({query: {x: {$gt: 1}, a: 2}, update: {$inc: {x: 1}}, new: true})
                  });

    // Verify mapReduce is using the partial index.
    var mapFunc = function () { emit(this._id, 1); };
    var reduceFunc = function (keyId, countArray) { return Array.sum(countArray); };

    ret = coll.mapReduce(mapFunc, reduceFunc, {out: "inline", query: {x: {$gt: 1}, a: 1}});
    assert.eq(1, ret.counts.input);

    // Distinct and count have special query paths, but can't take hint.
    ret = coll.distinct("a", {x: {$gt: 1}, a: 1});
    assert.eq(1, ret.length);

    ret = coll.count({x: {$gt: 1}, a: 1});
    assert.eq(1, ret);

    // Verify findAndModify uses the partial index.
    ret = coll.findAndModify({query: {x: {$gt: 1}, a: 1},
                              update: {$inc: {x: 1}}, new: true});
    assert.eq(2, ret._id);
    assert.eq(1, getNumKeys("x_1"));


    ret = coll.findAndModify({query:{x:{$gt:6}, a: 1},
                              update: {$inc: {x: 1}}, new: true});
    assert.eq(2, ret._id);
    assert.eq(1, getNumKeys("x_1"));

    // Check that aggregate is using the partial index.
    ret = coll.aggregate([{$match: {x: {$gt: 1}, a: 1}},
                          {$group: {_id:1, count: {$sum: 1}}}]).next();
    assert.eq(1, ret.count);
})();
