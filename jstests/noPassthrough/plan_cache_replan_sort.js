/**
 * Test that when replanning happens due to blocking sort's memory limit, we include
 * replanReason: "cached plan returned: ..." in the profiling data.
 * @tags: [requires_profiling]
 */
(function() {
    "use strict";

    load("jstests/libs/profiler.js");

    const conn = MongoRunner.runMongod({
        // Disallow blocking sort, by setting the memory limit very low (1 byte).
        setParameter: "internalQueryExecMaxBlockingSortBytes=1"
    });
    const db = conn.getDB("test");
    const coll = db.plan_cache_replan_sort;
    coll.drop();

    // Ensure a plan with a sort stage gets cached.
    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({y: 1}));
    const docs = Array.from({length: 20}, (_, i) => ({x: 7, y: i}));
    assert.commandWorked(db.runCommand({insert: coll.getName(), documents: docs}));
    // { x: 5 } is very selective because it matches 0 documents, so the x index will win.
    // This will succeed despite the low memory limit on sort, because the sort stage will see zero
    // documents.
    assert.eq(0, coll.find({x: 5}).sort({y: 1}).itcount());

    const cachedPlans = coll.getPlanCache().getPlansByQuery({x: 5}, {}, {y: 1}).plans;
    assert.lte(1, cachedPlans.length);
    const winningPlan = cachedPlans[0];
    assert.eq("SORT", winningPlan.reason.stats.stage);

    // Assert we "replan", by running the same query with different parameters.
    // This time the filter is not selective at all.
    db.setProfilingLevel(2);
    assert.eq(20, coll.find({x: 7}).sort({y: 1}).itcount());

    const profileObj = getLatestProfilerEntry(db, {op: "query"});
    assert.eq(profileObj.ns, coll.getFullName());
    assert.eq(profileObj.replanned, true);
    assert.eq(
        profileObj.replanReason,
        "cached plan returned: OperationFailed: Sort operation used more than the maximum 1 bytes of " +
            "RAM. Add an index, or specify a smaller limit.");

    MongoRunner.stopMongod(conn);
}());
