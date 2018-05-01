// Test that the explain will use backup plan if the original winning plan ran out of memory in the
// "executionStats" mode
// This test was designed to reproduce SERVER-32721"

load("jstests/libs/analyze_plan.js");
(function() {
    "use strict";

    const explain_backup_plan_test = db.explain_backup_plan;
    explain_backup_plan_test.drop();

    let bulk = explain_backup_plan_test.initializeUnorderedBulkOp();

    for (let i = 0; i < 100000; ++i) {
        bulk.insert({_id: i, x: i, y: i});
    }

    bulk.execute();
    explain_backup_plan_test.ensureIndex({x: 1});

    db.adminCommand({setParameter: 1, internalQueryExecMaxBlockingSortBytes: 100});

    const test1 = explain_backup_plan_test.find({x: {$gte: 90}}).sort({_id: 1}).explain(true);
    const test2 = explain_backup_plan_test.find({x: {$gte: 90000}}).sort({_id: 1}).explain(true);
    // This query will not use the backup plan, hence it generates only two stages: winningPlan and
    // rejectedPlans.
    assert(!backupPlanUsed(test1), "test1 did not use a backup plan");
    // This query will use backup plan, the exaplin output for this query will generate three
    // stages: winningPlan, rejectedPlans and originalWinningPlan.
    assert(backupPlanUsed(test2), "backup plan invoked in test2");
})();