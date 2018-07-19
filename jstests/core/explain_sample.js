/**
 * Tests that explaining an aggregation pipeline which uses the $sample stage will fail on WT and
 * mmapv1. We only check these storage engines since they use a storage-engine supported random
 * cursor for $sample.
 *
 * Does not support stepdowns since the test tries to run an aggregation command with explain
 * which may lead to incomplete results being returned.
 * @tags: [does_not_support_stepdowns]
 */
(function() {
    load("jstests/libs/analyze_plan.js");

    if (jsTest.options().storageEngine && jsTest.options().storageEngine !== "wiredTiger" &&
        jsTest.options().storageEngine !== "mmapv1") {
        jsTest.log("Skipping for non-WT/mmapv1 storage engine: " + jsTest.options().storageEngine);
        return;
    }

    const coll = db.explain_sample;
    coll.drop();

    for (let i = 0; i < 1000; ++i) {
        assert.writeOK(coll.insert({a: 1}));
    }

    const pipeline = {$sample: {size: 10}};
    const explain = assert.commandWorked(coll.explain(false).aggregate(pipeline));

    // Check that a random cursor is used.
    const stages = getAggPlanStages(explain, "$sampleFromRandomCursor");
    assert.gt(stages.length, 0);

    // Check that running the same pipeline with an explain of 'executionStats' or
    // 'allPlansExecution' fails.
    assert.throws(() => coll.explain("executionStats").aggregate(pipeline));
    assert.throws(() => coll.explain("allPlansExecution").aggregate(pipeline));
})();
