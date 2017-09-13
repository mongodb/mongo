// Test partial indexes with commands that don't use explain.  These commands are tested against
// mongod with the --notablescan flag set, so that they fail if the index is not used.
(function() {
    "use strict";
    var runner = MongoRunner.runMongod({setParameter: "notablescan=1"});
    var coll = runner.getDB("test").index_partial_no_explain_cmds;
    var ret;

    coll.drop();

    assert.commandWorked(coll.ensureIndex({x: 1}, {partialFilterExpression: {a: 1}}));

    assert.writeOK(coll.insert({_id: 1, x: 5, a: 2}));  // Not in index.
    assert.writeOK(coll.insert({_id: 2, x: 6, a: 1}));  // In index.

    // Verify we will throw if the partial index can't be used.
    assert.throws(function() {
        coll.find({x: {$gt: 1}, a: 2}).itcount();
    });

    //
    // Test mapReduce.
    //

    var mapFunc = function() {
        emit(this._id, 1);
    };
    var reduceFunc = function(keyId, countArray) {
        return Array.sum(countArray);
    };

    ret = coll.mapReduce(mapFunc, reduceFunc, {out: "inline", query: {x: {$gt: 1}, a: 1}});
    assert.eq(1, ret.counts.input);

    //
    // Test distinct.
    //

    ret = coll.distinct("a", {x: {$gt: 1}, a: 1});
    assert.eq(1, ret.length);
    ret = coll.distinct("x", {x: {$gt: 1}, a: 1});
    assert.eq(1, ret.length);
    assert.throws(function() {
        printjson(coll.distinct("a", {a: 0}));
    });
    assert.throws(function() {
        printjson(coll.distinct("x", {a: 0}));
    });

    // SERVER-19511 regression test: distinct with no query predicate should return the correct
    // number of results.  This query should not be allowed to use the partial index, so it should
    // use a collection scan instead.  Although this test enables --notablescan, this does not cause
    // operations to fail if they have no query predicate.
    ret = coll.distinct("x");
    assert.eq(2, ret.length);
})();
