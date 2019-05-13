/**
 * Tests that each of the update shell helpers correctly validates pipeline-style update operations.
 *
 * This test is tagged as 'requires_find_command' to exclude it from the legacy passthroughs, since
 * pipeline syntax cannot be used for OP_UPDATE requests.
 *
 * @tags: [requires_find_command, requires_non_retryable_writes, assumes_write_concern_unchanged]
 */
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For 'arrayEq'.

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
    const findAndModifyPostImage = testColl.findAndModify(
        {query: {_id: 1}, update: [{$set: {findAndModify: true}}], new: true});
    assert.docEq(findAndModifyPostImage, expectedFindAndModifyPostImage);
    const findOneAndUpdatePostImage = testColl.findOneAndUpdate(
        {_id: 1}, [{$set: {findOneAndUpdate: true}}], {returnNewDocument: true});
    assert.docEq(findOneAndUpdatePostImage, expectedFindOneAndUpdatePostImage);

    // Shell helpers for replacement updates should reject pipeline-style updates.
    assert.throws(() => testColl.replaceOne({_id: 1}, [{$replaceRoot: {newRoot: {}}}]));
    assert.throws(() => testColl.findOneAndReplace({_id: 1}, [{$replaceRoot: {newRoot: {}}}]));
    assert.throws(
        () => testColl.bulkWrite(
            [{replaceOne: {filter: {_id: 1}, replacement: [{$replaceRoot: {newRoot: {}}}]}}]));
})();
