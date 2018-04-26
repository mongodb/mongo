// Test that the explain will use backup plan if the original winning plan ran out of memory in the
// "executionStats" mode
// This test was designed to reproduce SERVER-32721"

load("jstests/libs/analyze_plan.js");

"use strict";

db.foo.drop();
let bulk = db.foo.initializeUnorderedBulkOp();

for (let i = 0; i < 100000; ++i) {
    bulk.insert({_id: i, x: i, y: i});
}

bulk.execute();
db.foo.ensureIndex({x: 1});

// Configure log level and lower the sort bytes limit.
db.setLogLevel(5, "query");
db.adminCommand({setParameter: 1, internalQueryExecMaxBlockingSortBytes: 100});

const test1 = db.foo.find({x: {$gte: 90}}).sort({_id: 1}).explain(true);
const test2 = db.foo.find({x: {$gte: 90000}}).sort({_id: 1}).explain(true);
// This query will not use the backup plan, hence it generates only two stages: winningPlan and
// rejectedPlans
assert.eq(backupPlanUsed(test1), false, "test1 did not use a backup plan");
// This query will use backup plan, the exaplin output for this query will generate three
// stages: winningPlan, rejectedPlans and originalWinningPlan
assert(backupPlanUsed(test2), "backup plan invoked in test2");