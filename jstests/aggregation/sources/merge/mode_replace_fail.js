// Tests the behaviour of the $merge stage with whenMatched=replace and whenNotMatched=fail.
//
// Cannot implicitly shard accessed collections because a collection can be implictly created and
// exists when none is expected.
(function() {
"use strict";

load("jstests/aggregation/extras/merge_helpers.js");  // For dropWithoutImplicitRecreate.
load("jstests/aggregation/extras/utils.js");          // For assertArrayEq.
load("jstests/libs/fixture_helpers.js");              // For FixtureHelpers.isMongos.

const source = db[`${jsTest.name()}_source`];
source.drop();
const target = db[`${jsTest.name()}_target`];
target.drop();
const mergeStage = {
    $merge: {into: target.getName(), whenMatched: "replace", whenNotMatched: "fail"}
};
const pipeline = [mergeStage];

// Test $merge when some documents in the source collection don't have a matching document in
// the target collection.
(function testMergeFailsIfMatchingDocumentNotFound() {
    // Single document without a match.
    assert.commandWorked(source.insert([{_id: 1, a: 1}, {_id: 2, a: 2}, {_id: 3, a: 3}]));
    assert.commandWorked(target.insert([{_id: 1, b: 1}, {_id: 3, b: 3}]));
    let error = assert.throws(() => source.aggregate(pipeline));
    assert.commandFailedWithCode(error, ErrorCodes.MergeStageNoMatchingDocument);
    assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, a: 1}, {_id: 3, a: 3}]});

    // Multiple documents without a match.
    assert(target.drop());
    assert.commandWorked(target.insert([{_id: 1, b: 1}]));
    error = assert.throws(() => source.aggregate(pipeline));
    assert.commandFailedWithCode(error, ErrorCodes.MergeStageNoMatchingDocument);
    assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, a: 1}]});
})();

// Test $merge when all documents in the source collection have a matching document in the
// target collection.
(function testMergeWhenAllDocumentsHaveMatch() {
    // Source has a single element with a match in the target.
    assert(source.drop());
    assert(target.drop());
    assert.commandWorked(source.insert({_id: 3, a: 3}));
    assert.commandWorked(target.insert([{_id: 1, b: 1}, {_id: 3, b: 3}]));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, b: 1}, {_id: 3, a: 3}]});

    // Source has multiple documents with matches in the target.
    assert(target.drop());
    assert.commandWorked(source.insert([{_id: 1, a: 1}, {_id: 2, a: 2}]));
    assert.commandWorked(target.insert([{_id: 1, b: 1}, {_id: 2, b: 2}, {_id: 3, b: 3}]));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [{_id: 1, a: 1}, {_id: 2, a: 2}, {_id: 3, a: 3}]
    });
})();

// Test $merge when the source collection is empty. The target collection should not be
// modified.
(function testMergeWhenSourceIsEmpty() {
    assert.commandWorked(source.deleteMany({}));
    assert(target.drop());
    assert.commandWorked(target.insert([{_id: 1, b: 1}, {_id: 2, b: 2}]));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, b: 1}, {_id: 2, b: 2}]});
})();

// Test $merge uses unorderded batch update. When a mismatch is detected in a batch, the error
// should be returned once the batch is processed and no further documents should be processed
// and updated.
(function testMergeUnorderedBatchUpdate() {
    const maxBatchSize = 16 * 1024 * 1024;  // 16MB
    const docSize = 1024 * 1024;            // 1MB
    const numDocs = 20;
    const maxDocsInBatch = maxBatchSize / docSize;

    assert(source.drop());
    dropWithoutImplicitRecreate(target.getName());

    // Insert 'numDocs' documents of size 'docSize' into the source collection.
    generateCollection({coll: source, numDocs: numDocs, docSize: docSize});

    // Copy over documents from the source collection into the target and remove the 'padding'
    // field from the projection, so we can distinguish which documents have been modified by
    // the $merge stage.
    assert.doesNotThrow(() =>
                            source.aggregate([{$project: {padding: 0}}, {$out: target.getName()}]));

    // Remove one document from the target collection so that $merge fails. This document should
    // be in the first batch of the aggregation pipeline below, which sorts documents by the _id
    // field in ascending order. Since each document in the source collection is 1MB, and the
    // max batch size is 16MB, the first batch will contain documents with the _id in the range
    // of [0, 15].
    assert.commandWorked(target.deleteOne({_id: Math.floor(Math.random() * maxDocsInBatch)}));

    // Ensure the target collection has 'numDocs' - 1 documents without the 'padding' field.
    assert.eq(numDocs - 1, target.find({padding: {$exists: false}}).itcount());

    // Run the $merge pipeline and ensure it fails, as there is one document in the source
    // collection without a match in the target.
    const error = assert.throws(() => source.aggregate([{$sort: {_id: 1}}, mergeStage]));
    assert.commandFailedWithCode(error, ErrorCodes.MergeStageNoMatchingDocument);

    // There will be maxDocsInBatch documents in the batch, one without a match.
    const numDocsModified = maxDocsInBatch - 1;
    // All remaining documents except those in the first batch must be left unmodified.
    const numDocsUnmodified = numDocs - maxDocsInBatch;
    assert.eq(numDocsModified, target.find({padding: {$exists: true}}).itcount());
    assert.eq(numDocsUnmodified, target.find({padding: {$exists: false}}).itcount());
})();
}());
