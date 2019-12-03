// Tests that certain aggregation operators have configurable memory limits.
(function() {
"use strict";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");
const coll = db.agg_configurable_memory_limit;

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 100; i++) {
    bulk.insert({_id: i, x: i, y: ["string 1", "string 2", "string 3", "string 4", "string " + i]});
}
assert.commandWorked(bulk.execute());

// Test that pushing a bunch of strings to an array does not exceed the default 100MB memory limit.
assert.doesNotThrow(
    () => coll.aggregate([{$unwind: "$y"}, {$group: {_id: null, strings: {$push: "$y"}}}]));

// Now lower the limit to test that it's configuration is obeyed.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxPushBytes: 100}));
let error = assert.throws(
    () => coll.aggregate([{$unwind: "$y"}, {$group: {_id: null, strings: {$push: "$y"}}}]));
assert.eq(error.code, ErrorCodes.ExceededMemoryLimit);

// Test that using $addToSet behaves similarly.
assert.doesNotThrow(
    () => coll.aggregate([{$unwind: "$y"}, {$group: {_id: null, strings: {$addToSet: "$y"}}}]));

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxAddToSetBytes: 100}));
error = assert.throws(
    () => coll.aggregate([{$unwind: "$y"}, {$group: {_id: null, strings: {$addToSet: "$y"}}}]));
assert.eq(error.code, ErrorCodes.ExceededMemoryLimit);

MongoRunner.stopMongod(conn);
}());
