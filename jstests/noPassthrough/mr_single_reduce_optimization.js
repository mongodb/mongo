(function() {
"use strict";
// See SERVER-68766. Verify that the reduce function is not run on a single value if the relevant
// flag is enabled.

const conn = MongoRunner.runMongod({setParameter: {mrEnableSingleReduceOptimization: true}});
const testDB = conn.getDB('foo');
const coll = testDB.bar;

assert.commandWorked(coll.insert({x: 1}));

const map = function() {
    emit(0, "mapped value");
};

const reduce = function(key, values) {
    return "reduced value";
};

let res = assert.commandWorked(
    testDB.runCommand({mapReduce: 'bar', map: map, reduce: reduce, out: {inline: 1}}));
assert.eq(res.results[0], {_id: 0, value: "mapped value"});
assert.commandWorked(coll.insert({x: 2}));
res = assert.commandWorked(
    testDB.runCommand({mapReduce: 'bar', map: map, reduce: reduce, out: {inline: 1}}));
assert.eq(res.results[0], {_id: 0, value: "reduced value"});

MongoRunner.stopMongod(conn);
}());
