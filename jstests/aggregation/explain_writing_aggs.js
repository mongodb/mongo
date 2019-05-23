/**
 * Test aggregation explain of $merge and $out pipelines.
 *
 * The $out stage is not allowed with a sharded output collection. Explain of $out or $merge does
 * not accept writeConcern.
 * @tags: [assumes_unsharded_collection, assumes_write_concern_unchanged]
 */
(function() {
    "use strict";

    load("jstests/libs/fixture_helpers.js");            // For FixtureHelpers.isMongos().
    load("jstests/libs/analyze_plan.js");               // For getAggPlanStage().
    load("jstests/aggregation/extras/out_helpers.js");  // For withEachMergeMode().

    let sourceColl = db.explain_writing_aggs_source;
    let targetColl = db.explain_writing_aggs_target;
    sourceColl.drop();
    targetColl.drop();

    assert.writeOK(sourceColl.insert({_id: 1}));

    // Test that $out can be explained with 'queryPlanner' explain verbosity and does not perform
    // any writes.
    let explain = sourceColl.explain("queryPlanner").aggregate([{$out: targetColl.getName()}]);
    let outExplain = getAggPlanStage(explain, "$out");
    assert.neq(outExplain, null, explain);
    assert.eq(outExplain.$out, targetColl.getName(), explain);
    assert.eq(targetColl.find().itcount(), 0, explain);

    // Test each $merge mode with 'queryPlanner' explain verbosity.
    withEachMergeMode(function({whenMatchedMode, whenNotMatchedMode}) {
        const mergeStage = {
            $merge: {
                into: targetColl.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode
            }
        };
        const explain = sourceColl.explain("queryPlanner").aggregate([mergeStage]);
        const mergeExplain = getAggPlanStage(explain, "$merge");
        assert.neq(mergeExplain, null, explain);
        assert(mergeExplain.hasOwnProperty("$merge"), explain);
        assert.eq(mergeExplain.$merge.whenMatched, whenMatchedMode, mergeExplain);
        assert.eq(mergeExplain.$merge.whenNotMatched, whenNotMatchedMode, mergeExplain);
        assert.eq(mergeExplain.$merge.on, "_id", mergeExplain);
        assert.eq(targetColl.find().itcount(), 0, explain);
    });

    function assertExecutionExplainFails(writingStage, verbosity) {
        assert.commandFailedWithCode(db.runCommand({
            explain: {aggregate: sourceColl.getName(), pipeline: [writingStage], cursor: {}},
            verbosity: verbosity
        }),
                                     [51029, 51184]);
        assert.eq(targetColl.find().itcount(), 0);
    }

    // Test that 'executionStats' and 'allPlansExec' level explain fail with each $merge mode. These
    // explain modes must fail, since they would attempt to do writes. Explain must always be
    // read-only (including explain of update and delete, which describe what writes they _would_ do
    // if exected for real).
    withEachMergeMode(function({whenMatchedMode, whenNotMatchedMode}) {
        const mergeStage = {
            $merge: {
                into: targetColl.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode
            }
        };
        assertExecutionExplainFails(mergeStage, "executionStats");
        assertExecutionExplainFails(mergeStage, "allPlansExecution");
    });

    // Also test the $out stage since it also performs writes.
    assertExecutionExplainFails({$out: targetColl.getName()}, "executionStats");
    assertExecutionExplainFails({$out: targetColl.getName()}, "allPlansExecution");

    // Execution explain should fail even if the source collection does not exist.
    sourceColl.drop();
    withEachMergeMode(function({whenMatchedMode, whenNotMatchedMode}) {
        const mergeStage = {
            $merge: {
                into: targetColl.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode
            }
        };
        assertExecutionExplainFails(mergeStage, "executionStats");
        assertExecutionExplainFails(mergeStage, "allPlansExecution");
    });

    // Also test the $out stage since it also performs writes.
    assertExecutionExplainFails({$out: targetColl.getName()}, "executionStats");
    assertExecutionExplainFails({$out: targetColl.getName()}, "allPlansExecution");
}());
