/**
 * Regression test confirming unique index constraints are upheld. See SERVER-58943 for details.
 *
 * @tags: [
 *      cannot_create_unique_index_when_using_hashed_shard_key,
 *      does_not_support_transactions
 *  ]
 */
(function() {
"use strict";
const dbName = "unique_index_insert";
const collName = "test";
const testDB = db.getSiblingDB("unique_index_insert");

testDB[collName].drop();
const coll = testDB[collName];

assert.commandWorked(coll.createIndex({i: 1, x: -1}, {unique: true}));

const n = 100;
const xStr = "x".repeat(50000);

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < n; i++) {
    bulk.insert({i, x: xStr});
}
assert.commandWorked(bulk.execute());

assert.eq(coll.find().itcount(), 100);

const maxIters = 2;
for (let iter = 0; iter < maxIters; iter++) {
    for (let i = 0; i < n; i++) {
        assert.commandFailed(coll.insert({i, x: xStr}), `Failed with i ${i}, iter ${iter}`);
    }
}
}());
