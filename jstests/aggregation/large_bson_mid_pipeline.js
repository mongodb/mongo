/**
 * Tests that extra-large BSON objects (>16MB) can be materialized for the '$match' stage in the
 * middle of the query plan without throwing 'BSONObjectTooLarge' exception.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For 'getAggPlanStage()'.

const testDB = db.getSiblingDB("jsTestName");
assert.commandWorked(testDB.dropDatabase());

const coll = testDB.coll;
const largeString = 'x'.repeat(10 * 1024 * 1024);
assert.commandWorked(coll.insert({a: 1, b: largeString}));

// Use '$addFields' to create extra-large documents in the middle of the pipeline followed by
// '$match'. Use '$_internalInhibitOptimization' to ensure '$match' is not removed by the optimizer.
const pipeline = [
    {$_internalInhibitOptimization: {}},
    {$addFields: {c: {$concat: ["$b", "-"]}}},
    {$match: {c: {$exists: true}}},
    {$project: {a: 1}}
];

assert.doesNotThrow(() => coll.aggregate(pipeline).toArray());
})();
