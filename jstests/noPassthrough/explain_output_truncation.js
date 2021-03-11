/**
 * Test that explain output is correctly truncated when it grows too large.
 */
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");

    const dbName = "test";
    const collName = jsTestName();
    const explainSizeParam = "internalQueryExplainSizeThresholdBytes";

    const conn = MongoRunner.runMongod({});
    assert.neq(conn, null, "mongod failed to start up");

    const testDb = conn.getDB(dbName);
    const coll = testDb[collName];
    coll.drop();

    assert.commandWorked(coll.createIndex({a: 1}));

    // Explain output should show a simple IXSCAN => FETCH => SORT_KEY_GENERATOR => SORT plan with
    // no truncation.
    let explain = coll.find({a: 1, b: 1}).sort({c: 1}).explain();
    let sortStage = getPlanStage(explain.queryPlanner.winningPlan, "SORT");
    assert.neq(sortStage, null, explain);
    let sortKeyGenStage = getPlanStage(explain.queryPlanner.winningPlan, "SORT_KEY_GENERATOR");
    assert.neq(sortKeyGenStage, null, explain);
    let fetchStage = getPlanStage(sortKeyGenStage, "FETCH");
    assert.neq(fetchStage, null, explain);
    let ixscanStage = getPlanStage(sortStage, "IXSCAN");
    assert.neq(ixscanStage, null, explain);

    // Calculate the size of explain output without the index scan. If the explain size threshold is
    // set near this amount, then the IXSCAN stage will need to be truncated.
    assert.neq(ixscanStage, null, explain);
    const newExplainSize =
        Object.bsonsize(explain.queryPlanner) - Object.bsonsize(ixscanStage) - 10;
    assert.gt(newExplainSize, 0);

    // Reduce the size at which we start truncating explain output. If we explain the same query
    // again, then the FETCH stage should be present, but the IXSCAN stage should be truncated.
    assert.commandWorked(
        testDb.adminCommand({setParameter: 1, [explainSizeParam]: newExplainSize}));

    explain = coll.find({a: 1, b: 1}).sort({c: 1}).explain();
    assert(planHasStage(testDb, explain.queryPlanner.winningPlan, "SORT"), explain);
    assert(planHasStage(testDb, explain.queryPlanner.winningPlan, "SORT_KEY_GENERATOR"), explain);
    fetchStage = getPlanStage(explain.queryPlanner.winningPlan, "FETCH");
    assert.neq(fetchStage, null, explain);
    assert(fetchStage.hasOwnProperty("inputStage"), explain);
    assert(fetchStage.inputStage.hasOwnProperty("warning"), explain);
    assert.eq(
        fetchStage.inputStage.warning, "stats tree exceeded BSON size limit for explain", explain);
    assert(!planHasStage(testDb, explain, "IXSCAN"), explain);

    MongoRunner.stopMongod(conn);
}());
