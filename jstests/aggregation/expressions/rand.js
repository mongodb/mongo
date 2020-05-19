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

// Test we have a per-document rand function.
const explain = coll.explain().aggregate(randPipeline);
const explainRand = getAggPlanStage(explain, "PROJECTION_DEFAULT");
assert.neq(null, explainRand, explain);
assert.eq(false, explainRand.transformBy.r.$rand.const, explain);

const resultArray = coll.aggregate(randPipeline).toArray();
assert.eq(1, resultArray.length);
const avg = resultArray[0]["avg"];

print("Average: ", avg);
// For continuous uniform distribution [0.0, 1.0] the variance is 1/12.
// Test certainty within 10 standard deviations.
const err = 10.0 / Math.sqrt(12.0 * N);
assert.lte(0.5 - err, avg);
assert.gte(0.5 + err, avg);

const collConst = db.expression_rand_const;
collConst.drop();
assert.commandWorked(collConst.insert({_id: i, v: 0}));

const randPipelineConst = [{$project: {r: {$rand: {const : true}}}}];

// Test rand is replaced with a constant
const explainConst = collConst.explain().aggregate(randPipelineConst);
const explainConstConst = getAggPlanStage(explainConst, "PROJECTION_DEFAULT");
const c = explainConstConst.transformBy.r.$const;
assert.lte(0.0, c, explain);
assert.gte(1.0, c, explain);

let sum = 0.0;
for (i = 0; i < N; i++) {
    const resultArrayConst = collConst.aggregate(randPipelineConst).toArray();
    assert.eq(1, resultArrayConst.length);

    const r = resultArrayConst[0]["r"];
    assert.lte(0.0, r);
    assert.gte(1.0, r);

    sum += r;
}

const avgConst = sum / N;
print("Average Const: ", avgConst);
assert.lte(0.5 - err, avgConst);
assert.gte(0.5 + err, avgConst);
}());
