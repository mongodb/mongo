/**
 * Test that a $lookup correctly optimizes a foreign pipeline containing a $sort and a $limit. This
 * test is designed to reproduce SERVER-36715.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");     // For getAggPlanStages().
load("jstests/libs/fixture_helpers.js");  // For isSharded.

const testDB = db.getSiblingDB("lookup_sort_limit");
testDB.dropDatabase();

const localColl = testDB.getCollection("local");
const fromColl = testDB.getCollection("from");

// Do not run the rest of the tests if the foreign collection is implicitly sharded but the flag to
// allow $lookup/$graphLookup into a sharded collection is disabled.
const getShardedLookupParam = db.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
const isShardedLookupEnabled = getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
    getShardedLookupParam.featureFlagShardedLookup.value;
if (FixtureHelpers.isSharded(fromColl) && !isShardedLookupEnabled) {
    return;
}

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
