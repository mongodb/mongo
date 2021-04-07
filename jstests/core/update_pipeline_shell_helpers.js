/**
 * Tests that each of the update shell helpers correctly validates pipeline-style update operations.
 *
 * This test is tagged as 'requires_find_command' to exclude it from the legacy passthroughs, since
 * pipeline syntax cannot be used for OP_UPDATE requests.
 *
 * @tags: [
 *   requires_find_command,
 *   requires_multi_updates,
 *   requires_non_retryable_writes,
 *   assumes_write_concern_unchanged,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For 'arrayEq'.
load("jstests/libs/analyze_plan.js");         // For planHasStage().
load("jstests/libs/fixture_helpers.js");      // For isMongos().

// Make sure that the test collection is empty before starting the test.
const testColl = db.update_pipeline_shell_helpers_test;
testColl.drop();

// Insert some test documents.
assert.commandWorked(testColl.insert({_id: 1, a: 1, b: 2}));
assert.commandWorked(testColl.insert({_id: 2, a: 2, b: 3}));

// Test that each of the update shell helpers permits pipeline-style updates.
assert.commandWorked(testColl.update({_id: 1}, [{$set: {update: true}}]));
assert.commandWorked(testColl.update({}, [{$set: {updateMulti: true}}], {multi: true}));
assert.commandWorked(testColl.updateOne({_id: 1}, [{$set: {updateOne: true}}]));
assert.commandWorked(testColl.updateMany({}, [{$set: {updateMany: true}}]));
assert.commandWorked(testColl.bulkWrite([
    {updateOne: {filter: {_id: 1}, update: [{$set: {bulkWriteUpdateOne: true}}]}},
    {updateMany: {filter: {}, update: [{$set: {bulkWriteUpdateMany: true}}]}}
]));

// Test that each of the Bulk API update functions correctly handle pipeline syntax.
const unorderedBulkOp = testColl.initializeUnorderedBulkOp();
const orderedBulkOp = testColl.initializeOrderedBulkOp();

unorderedBulkOp.find({_id: 1}).updateOne([{$set: {unorderedBulkOpUpdateOne: true}}]);
unorderedBulkOp.find({}).update([{$set: {unorderedBulkOpUpdateMulti: true}}]);
orderedBulkOp.find({_id: 1}).updateOne([{$set: {orderedBulkOpUpdateOne: true}}]);
orderedBulkOp.find({}).update([{$set: {orderedBulkOpUpdateMulti: true}}]);
assert.commandWorked(unorderedBulkOp.execute());
assert.commandWorked(orderedBulkOp.execute());

// Verify that the results of the various update operations are as expected.
const observedResults = testColl.find().toArray();
const expectedResults = [
    {
        _id: 1,
        a: 1,
        b: 2,
        update: true,
        updateMulti: true,
        updateOne: true,
        updateMany: true,
        bulkWriteUpdateOne: true,
        bulkWriteUpdateMany: true,
        unorderedBulkOpUpdateOne: true,
        unorderedBulkOpUpdateMulti: true,
        orderedBulkOpUpdateOne: true,
        orderedBulkOpUpdateMulti: true
    },
    {
        _id: 2,
        a: 2,
        b: 3,
        updateMulti: true,
        updateMany: true,
        bulkWriteUpdateMany: true,
        unorderedBulkOpUpdateMulti: true,
        orderedBulkOpUpdateMulti: true
    }
];
assert(arrayEq(observedResults, expectedResults));

// Test that findAndModify and associated helpers correctly handle pipeline syntax.
const expectedFindAndModifyPostImage = Object.merge(expectedResults[0], {findAndModify: true});
const expectedFindOneAndUpdatePostImage =
    Object.merge(expectedFindAndModifyPostImage, {findOneAndUpdate: true});
const findAndModifyPostImage =
    testColl.findAndModify({query: {_id: 1}, update: [{$set: {findAndModify: true}}], new: true});
assert.docEq(findAndModifyPostImage, expectedFindAndModifyPostImage);
const findOneAndUpdatePostImage = testColl.findOneAndUpdate(
    {_id: 1}, [{$set: {findOneAndUpdate: true}}], {returnNewDocument: true});
assert.docEq(findOneAndUpdatePostImage, expectedFindOneAndUpdatePostImage);

//
// Explain for updates that use an _id lookup query.
//
{
    let explain = testColl.explain("queryPlanner").update({_id: 2}, [{$set: {y: 999}}]);
    assert(planHasStage(db, explain.queryPlanner.winningPlan, "IDHACK"));
    assert(planHasStage(db, explain.queryPlanner.winningPlan, "UPDATE"));

    // Run explain with execution-level verbosity.
    explain = testColl.explain("executionStats").update({_id: 2}, [{$set: {y: 999}}]);
    assert.eq(explain.executionStats.totalDocsExamined, 1, explain);
    // UPDATE stage would modify one document.
    const updateStage = getPlanStage(explain.executionStats.executionStages, "UPDATE");
    assert.eq(updateStage.nWouldModify, 1);

    // Check that no write was performed.
    assert.eq(testColl.find({y: 999}).count(), 0);
}

//
// Explain for updates that use a query which requires a COLLSCAN.
//

// We skip these tests under sharded fixtures, since sharded passthroughs require that FAM queries
// contain the shard key.
if (!FixtureHelpers.isMongos(db)) {
    let explain = testColl.explain("queryPlanner").update({a: 2}, [{$set: {y: 999}}]);
    assert(planHasStage(db, explain.queryPlanner.winningPlan, "COLLSCAN"));
    assert(planHasStage(db, explain.queryPlanner.winningPlan, "UPDATE"));

    // Run explain with execution-level verbosity.
    explain = testColl.explain("executionStats").update({a: 2}, [{$set: {y: 999}}]);
    // UPDATE stage would modify one document.
    const updateStage = getPlanStage(explain.executionStats.executionStages, "UPDATE");
    assert.eq(updateStage.nWouldModify, 1);

    // Check that no write was performed.
    assert.eq(testColl.find({y: 999}).count(), 0);
}

// Shell helpers for replacement updates should reject pipeline-style updates.
assert.throws(() => testColl.replaceOne({_id: 1}, [{$replaceWith: {}}]));
assert.throws(() => testColl.findOneAndReplace({_id: 1}, [{$replaceWith: {}}]));
assert.throws(() => testColl.bulkWrite(
                  [{replaceOne: {filter: {_id: 1}, replacement: [{$replaceWith: {}}]}}]));
})();
