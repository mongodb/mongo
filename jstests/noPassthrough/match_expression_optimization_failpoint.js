// Tests that match expression optimization works properly when the failpoint isn't triggered, and
// is disabled properly when it is triggered.
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");  // For aggPlan functions.

    const kTestZip = 44100;
    const pipeline = [{$match: {_id: {$in: [kTestZip]}}}, {$sort: {_id: 1}}];

    const conn = MongoRunner.runMongod({});
    assert.neq(conn, null, `Mongod failed to start up.`);
    const testDb = conn.getDB("test");
    const coll = testDb.agg_opt;

    for (let i = 0; i < 25; ++i) {
        assert.commandWorked(coll.insert({
            _id: kTestZip + i,
            city: "Cleveland",
            pop: Math.floor(Math.random() * 100000) + 100,
            state: "OH"
        }));
    }

    const enabledPlan = coll.explain().aggregate(pipeline);
    // Test that a single equality condition $in was optimized to an $eq.
    assert.eq(getAggPlanStage(enabledPlan, "$cursor").$cursor.queryPlanner.parsedQuery._id.$eq,
              kTestZip);

    const enabledResult = coll.aggregate(pipeline);

    // Enable a failpoint that will cause match expression optimizations to be skipped. Test that
    // the expression isn't modified after it's specified.
    assert.commandWorked(testDb.adminCommand(
        {configureFailPoint: "disableMatchExpressionOptimization", mode: "alwaysOn"}));

    const disabledPlan = coll.explain().aggregate(pipeline);
    // Test that the $in query still exists and hasn't been optimized to an $eq.
    assert.eq(getAggPlanStage(disabledPlan, "$cursor").$cursor.queryPlanner.parsedQuery._id.$in,
              [kTestZip]);

    const disabledResult = coll.aggregate(pipeline);

    // Test that the result is the same with and without optimizations enabled (result is sorted).
    assert.eq(enabledResult, disabledResult);

    MongoRunner.stopMongod(conn);
}());
