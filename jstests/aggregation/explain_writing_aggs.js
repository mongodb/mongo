/**
 * Test aggregation explain of $merge and $out pipelines.
 *
 * The $out stage is not allowed with a sharded output collection. Explain of $out or $merge does
 * not accept writeConcern.
 * @tags: [
 *   assumes_unsharded_collection,
 *   assumes_write_concern_unchanged,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");              // For FixtureHelpers.isMongos().
load("jstests/libs/analyze_plan.js");                 // For getAggPlanStage().
load("jstests/aggregation/extras/merge_helpers.js");  // For withEachMergeMode().

let sourceColl = db.explain_writing_aggs_source;
let targetColl = db.explain_writing_aggs_target;
sourceColl.drop();
targetColl.drop();

assert.commandWorked(sourceColl.insert({_id: 1}));

// Verifies that running the execution explains do not error, perform any writes, or create the
// target collection.
function assertExecutionExplainOk(writingStage, verbosity) {
    assert.commandWorked(db.runCommand({
        explain: {aggregate: sourceColl.getName(), pipeline: [writingStage], cursor: {}},
        verbosity: verbosity
    }));
    assert.eq(targetColl.find().itcount(), 0);
    // Verify that the collection was not created.
    const collectionList = db.getCollectionInfos({name: targetColl.getName()});
    assert.eq(0, collectionList.length, collectionList);
}

// Test that $out can be explained with 'queryPlanner' explain verbosity and does not perform
// any writes.
let explain = sourceColl.explain("queryPlanner").aggregate([{$out: targetColl.getName()}]);
let outExplain = getAggPlanStage(explain, "$out");
assert.neq(outExplain, null, explain);

assert.eq(outExplain.$out.coll, targetColl.getName(), explain);
assert.eq(outExplain.$out.db, db.getName(), explain);
assert.eq(targetColl.find().itcount(), 0, explain);

// Verify that execution explains don't error for $out.
assertExecutionExplainOk({$out: targetColl.getName()}, "executionStats");
assertExecutionExplainOk({$out: targetColl.getName()}, "allPlansExecution");

// Test each $merge mode with each explain verbosity.
withEachMergeMode(function({whenMatchedMode, whenNotMatchedMode}) {
    const mergeStage = {
        $merge: {
            into: targetColl.getName(),
            whenMatched: whenMatchedMode,
            whenNotMatched: whenNotMatchedMode
        }
    };

    // Verify that execution explains don't error for $merge.
    assertExecutionExplainOk(mergeStage, "executionStats");
    assertExecutionExplainOk(mergeStage, "allPlansExecution");

    const explain = sourceColl.explain("queryPlanner").aggregate([mergeStage]);
    const mergeExplain = getAggPlanStage(explain, "$merge");
    assert.neq(mergeExplain, null, explain);
    assert(mergeExplain.hasOwnProperty("$merge"), explain);
    assert.eq(mergeExplain.$merge.whenMatched, whenMatchedMode, mergeExplain);
    assert.eq(mergeExplain.$merge.whenNotMatched, whenNotMatchedMode, mergeExplain);
    assert.eq(mergeExplain.$merge.on, "_id", mergeExplain);
    assert.eq(targetColl.find().itcount(), 0, explain);
});
}());
