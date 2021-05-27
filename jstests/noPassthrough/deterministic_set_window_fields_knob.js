/**
 * Test that $setWindowFields behaves deterministically if
 * internalQueryAppendIdToSetWindowFieldsSort is enabled.
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const db = conn.getDB(dbName);
assert.commandWorked(db.dropDatabase());
const collA = db.getCollection('a');
collA.createIndex({val: 1, a: 1});
let i = 0;
for (i = 0; i < 3; i++) {
    collA.insert({_id: i, val: 1, a: i});
}
const collB = db.getCollection('b');
collB.createIndex({val: 1, b: 1});
for (i = 0; i < 3; i++) {
    collB.insert({_id: i, val: 1, b: (3 - i)});
}

// This ensures that if the query knob is turned off (which it is by default) we get the results in
// a and b order respectively.
let resultA = collA
                  .aggregate([
                      {$setWindowFields: {sortBy: {val: 1}, output: {ids: {$push: "$_id"}}}},
                      {$limit: 1},
                      {$project: {_id: 0, ids: 1}}
                  ])
                  .toArray()[0];
let resultB = collB
                  .aggregate([
                      {$setWindowFields: {sortBy: {val: 1}, output: {ids: {$push: "$_id"}}}},
                      {$limit: 1},
                      {$project: {_id: 0, ids: 1}}
                  ])
                  .toArray()[0];

assert.eq(resultA["ids"], [0, 1, 2]);
assert.eq(resultB["ids"], [2, 1, 0]);

assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryAppendIdToSetWindowFieldsSort: true}));

// Because of the index resultA's ids array should be in a order and resultB's ids should be in b
// order unless the query knob is working properly.
resultA = collA
              .aggregate([
                  {$setWindowFields: {sortBy: {val: 1}, output: {ids: {$push: "$_id"}}}},
                  {$limit: 1},
                  {$project: {_id: 0, ids: 1}}
              ])
              .toArray()[0];
resultB = collB
              .aggregate([
                  {$setWindowFields: {sortBy: {val: 1}, output: {ids: {$push: "$_id"}}}},
                  {$limit: 1},
                  {$project: {_id: 0, ids: 1}}
              ])
              .toArray()[0];

// This assertion ensures that the results are the same.
assert.eq(resultA["ids"], resultB["ids"]);

MongoRunner.stopMongod(conn);
})();
