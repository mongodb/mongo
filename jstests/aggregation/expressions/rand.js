/**
 * Test the $rand expression.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getAggPlanStage().

const coll = db.expression_rand;
coll.drop();

print("Generating test collection...");
const N = 3000;
let i;
const bulk = coll.initializeUnorderedBulkOp();
for (i = 0; i < N; i++) {
    bulk.insert({_id: i, v: 0});
}
assert.commandWorked(bulk.execute());

const randPipeline = [{$project: {r: {$rand: {}}}}, {$group: {_id: 0, avg: {$avg: "$r"}}}];
const resultArray = coll.aggregate(randPipeline).toArray();
assert.eq(1, resultArray.length);
const avg = resultArray[0]["avg"];

print("Average: ", avg);
// For continuous uniform distribution [0.0, 1.0] the variance is 1/12 .
// Test certainty within 10 standard deviations.
const err = 10.0 / Math.sqrt(12.0 * N);
assert.lte(0.5 - err, avg);
assert.gte(0.5 + err, avg);
}());
