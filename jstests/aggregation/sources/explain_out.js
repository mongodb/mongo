/**
 * Test aggregation explain of $out pipelines.
 *
 * The 'replaceCollection' $out mode is not allowed with a sharded output collection. Explain of
 * $out does not accept writeConcern.
 * @tags: [assumes_unsharded_collection, assumes_write_concern_unchanged]
 */
(function() {
    "use strict";

    load("jstests/libs/fixture_helpers.js");            // For FixtureHelpers.isMongos().
    load("jstests/libs/analyze_plan.js");               // For getAggPlanStage().
    load("jstests/aggregation/extras/out_helpers.js");  // For withEachOutMode().

    // Mongos currently uses its own error code if any shard's explain fails.
    const kErrorCode = FixtureHelpers.isMongos(db) ? 17403 : 51029;

    let sourceColl = db.explain_out_source;
    let targetColl = db.explain_out_target;
    sourceColl.drop();
    targetColl.drop();

    assert.writeOK(sourceColl.insert({_id: 1}));

    function assertQueryPlannerExplainSucceeds(outStage) {
        let explain = sourceColl.explain("queryPlanner").aggregate([outStage]);
        let outExplain = getAggPlanStage(explain, "$out");
        assert.neq(outExplain, null, explain);
        assert.eq(targetColl.find().itcount(), 0, explain);
        return outExplain.$out;
    }

    // Test each out mode with 'queryPlanner' explain verbosity;
    withEachOutMode(function(outMode) {
        const outStage = {$out: {to: targetColl.getName(), mode: outMode}};
        const explain = sourceColl.explain("queryPlanner").aggregate([outStage]);
        const outExplain = getAggPlanStage(explain, "$out");
        assert.neq(outExplain, null, explain);
        assert(outExplain.hasOwnProperty("$out"), explain);
        assert.eq(outExplain.$out.mode, outMode, outExplain);
        assert.eq(outExplain.$out.uniqueKey, {_id: 1}, outExplain);
        assert.eq(targetColl.find().itcount(), 0, explain);
    });

    function assertExecutionExplainFails(outStage, verbosity) {
        assert.commandFailedWithCode(db.runCommand({
            explain: {aggregate: sourceColl.getName(), pipeline: [outStage], cursor: {}},
            verbosity: verbosity
        }),
                                     kErrorCode);
        assert.eq(targetColl.find().itcount(), 0);
    }

    // Test that 'executionStats' and 'allPlansExec' level explain fail with each $out mode. These
    // explain modes must fail, since they would attempt to do writes. Explain must always be
    // read-only (including explain of update and delete, which describe what writes they _would_ do
    // if exected for real).
    withEachOutMode(function(outMode) {
        const outStage = {$out: {to: targetColl.getName(), mode: outMode}};
        assertExecutionExplainFails(outStage, "executionStats");
        assertExecutionExplainFails(outStage, "allPlansExecution");
    });

    // Execution explain should fail even if the source collection does not exist.
    sourceColl.drop();
    withEachOutMode(function(outMode) {
        const outStage = {$out: {to: targetColl.getName(), mode: outMode}};
        assertExecutionExplainFails(outStage, "executionStats");
        assertExecutionExplainFails(outStage, "allPlansExecution");
    });
}());
