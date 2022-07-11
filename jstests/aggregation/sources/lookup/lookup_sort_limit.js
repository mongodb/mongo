/**
 * Test that a $lookup correctly optimizes a foreign pipeline containing a $sort and a $limit. This
 * test is designed to reproduce SERVER-36715.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getAggPlanStages().

const testDB = db.getSiblingDB("lookup_sort_limit");
testDB.dropDatabase();

const localColl = testDB.getCollection("local");
const fromColl = testDB.getCollection("from");

const bulk = fromColl.initializeUnorderedBulkOp();
for (let i = 0; i < 10; i++) {
    bulk.insert({_id: i, foreignField: i});
}
assert.commandWorked(bulk.execute());
assert.commandWorked(localColl.insert({_id: 0}));

let res = localColl
                  .aggregate([{
                      $lookup: {
                          from: fromColl.getName(),
                          let : {},
                          pipeline: [{$sort: {_id: 1}}, {$limit: 1}],
                          as: "result"
                      }
                  }])
                  .toArray();

assert.eq({_id: 0, result: [{_id: 0, foreignField: 0}]}, res[0]);

// Run a similar test except with a sort that cannot be covered with an index scan.
res = localColl
              .aggregate([{
                  $lookup: {
                      from: fromColl.getName(),
                      let : {},
                      pipeline: [{$sort: {foreignField: -1}}, {$limit: 1}],
                      as: "result"
                  }
              }])
              .toArray();

assert.eq({_id: 0, result: [{_id: 9, foreignField: 9}]}, res[0]);
}());
