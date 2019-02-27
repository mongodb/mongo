// Tests that match expression optimization works properly when the failpoint isn't triggered, and
// is disabled properly when it is triggered.
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");  // For aggPlan functions.
    Random.setRandomSeed();

    const conn = MongoRunner.runMongod({});
    assert.neq(conn, null, "Mongod failed to start up.");
    const testDb = conn.getDB("test");
    const coll = testDb.agg_opt;

    const kTestZip = 44100;
    for (let i = 0; i < 25; ++i) {
        assert.commandWorked(coll.insert(
            {_id: kTestZip + i, city: "Cleveland", pop: Random.randInt(100000), state: "OH"}));
    }

    const pipeline = [{$match: {_id: {$in: [kTestZip]}}}, {$sort: {_id: 1}}];

    const enabledPlan = coll.explain().aggregate(pipeline);
    // Test that a single equality condition $in was optimized to an $eq.
    assert.eq(enabledPlan.queryPlanner.parsedQuery._id.$eq, kTestZip);

    const enabledResult = coll.aggregate(pipeline).toArray();

    // Enable a failpoint that will cause match expression optimizations to be skipped.
    assert.commandWorked(testDb.adminCommand(
        {configureFailPoint: "disableMatchExpressionOptimization", mode: "alwaysOn"}));

    const disabledPlan = coll.explain().aggregate(pipeline);
    // Test that the $in query still exists and hasn't been optimized to an $eq.
    assert.eq(disabledPlan.queryPlanner.parsedQuery._id.$in, [kTestZip]);

    const disabledResult = coll.aggregate(pipeline).toArray();

    // Test that the result is the same with and without optimizations enabled (result is sorted).
    assert.eq(enabledResult, disabledResult);

    MongoRunner.stopMongod(conn);
}());
