// Test partial indexes with commands that don't use explain.  These commands are tested against
// mongod with the --notablescan flag set, so that they fail if the index is not used.
// @tags: [requires_scripting]
import {resultsEq} from "jstests/aggregation/extras/utils.js";

let runner = MongoRunner.runMongod({setParameter: "notablescan=1"});
const db = runner.getDB("test");
let coll = db[jsTestName()];
let ret;

coll.drop();
db.getCollection("mrOutput").drop();

assert.commandWorked(coll.createIndex({x: 1}, {partialFilterExpression: {a: 1}}));

assert.commandWorked(coll.insert({_id: 1, x: 5, a: 2})); // Not in index.
assert.commandWorked(coll.insert({_id: 2, x: 6, a: 1})); // In index.

// Verify we will throw if the partial index can't be used.
assert.throws(function () {
    coll.find({x: {$gt: 1}, a: 2}).itcount();
});

//
// Test mapReduce.
//

let mapFunc = function () {
    emit(this._id, 1);
};
let reduceFunc = function (keyId, countArray) {
    return Array.sum(countArray);
};

assert.commandWorked(coll.mapReduce(mapFunc, reduceFunc, {out: "mrOutput", query: {x: {$gt: 1}, a: 1}}));
assert(resultsEq([{"_id": 2, "value": 1}], db.getCollection("mrOutput").find().toArray()));

//
// Test distinct.
//

ret = coll.distinct("a", {x: {$gt: 1}, a: 1});
assert.eq(1, ret.length);
ret = coll.distinct("x", {x: {$gt: 1}, a: 1});
assert.eq(1, ret.length);
assert.throws(function () {
    printjson(coll.distinct("a", {a: 0}));
});
assert.throws(function () {
    printjson(coll.distinct("x", {a: 0}));
});

// SERVER-19511 regression test: distinct with no query predicate should return the correct
// number of results.  This query should not be allowed to use the partial index, so it should
// use a collection scan instead.  Although this test enables --notablescan, this does not cause
// operations to fail if they have no query predicate.
ret = coll.distinct("x");
assert.eq(2, ret.length);
MongoRunner.stopMongod(runner);
