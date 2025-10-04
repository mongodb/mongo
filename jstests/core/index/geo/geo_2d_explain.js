// @tags: [
//   assumes_balancer_off,
// ]
import {getExecutionStats, getPlanStages} from "jstests/libs/query/analyze_plan.js";

let t = db.geo_2d_explain;

t.drop();

let n = 1000;

// insert n documents with integer _id, a can be 1-5, loc is close to [40, 40]
t.drop();
t.createIndex({loc: "2d", _id: 1});

let x = 40;
let y = 40;
for (var i = 0; i < n; i++) {
    // random number in range [1, 5]
    let a = Math.floor(Math.random() * 5) + 1;
    let dist = 4.0;
    let dx = (Math.random() - 0.5) * dist;
    let dy = (Math.random() - 0.5) * dist;
    let loc = [x + dx, y + dy];
    t.save({_id: i, a: a, loc: loc});
}

let explain = t.find({loc: {$near: [40, 40]}, _id: {$lt: 50}}).explain("executionStats");

// On a sharded cluster, this test assumes that the cluster only has one shard.
let stats = getExecutionStats(explain)[0];
assert.eq(stats.nReturned, 50);
assert.lte(stats.nReturned, stats.totalDocsExamined);
assert.eq(stats.executionSuccess, true, "expected success: " + tojson(explain));

// Check for the existence of a indexVersion field in explain output.
let indexStages = getPlanStages(explain.queryPlanner.winningPlan, "GEO_NEAR_2D");
print(tojson(indexStages));
assert.gt(indexStages.length, 0);
for (var i = 0; i < indexStages.length; i++) {
    assert.gte(indexStages[i].indexVersion, 1);
}
