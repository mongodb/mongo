// Tests basic use cases for all $merge modes.
//
// Cannot implicitly shard accessed collections because a collection can be implictly created and
// exists when none is expected.
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.

const source = db.all_modes_source;
const target = db.all_modes_target;

(function setup() {
    source.drop();
    target.drop();

    // All tests use the same data in the source collection.
    assert.commandWorked(
        source.insert([{_id: 1, a: 1, b: "a"}, {_id: 2, a: 2, b: "b"}, {_id: 3, a: 3, b: "c"}]));
})();

// Test 'whenMatched=replace whenNotMatched=insert' mode. This is an equivalent of a
// replacement-style update with upsert=true.
(function testWhenMatchedReplaceWhenNotMatchedInsert() {
    assert.commandWorked(target.insert([{_id: 1, a: 10}, {_id: 3, a: 30}, {_id: 4, a: 40}]));
    assert.doesNotThrow(() => source.aggregate([
        {$merge: {into: target.getName(), whenMatched: "replace", whenNotMatched: "insert"}}
    ]));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, a: 1, b: "a"},
            {_id: 2, a: 2, b: "b"},
            {_id: 3, a: 3, b: "c"},
            {_id: 4, a: 40}
        ]
    });
})();

// Test 'whenMatched=replace whenNotMatched=fail' mode. For matched documents the update
// should be unordered and report an error at the end when all documents in a batch have been
// processed, it will not fail as soon as we hit the first document without a match.
(function testWhenMatchedReplaceWhenNotMatchedFail() {
    assert(target.drop());
    assert.commandWorked(target.insert([{_id: 1, a: 10}, {_id: 3, a: 30}, {_id: 4, a: 40}]));
    const error = assert.throws(
        () => source.aggregate(
            [{$merge: {into: target.getName(), whenMatched: "replace", whenNotMatched: "fail"}}]));
    assert.commandFailedWithCode(error, ErrorCodes.MergeStageNoMatchingDocument);
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [{_id: 1, a: 1, b: "a"}, {_id: 3, a: 3, b: "c"}, {_id: 4, a: 40}]
    });
})();

// Test 'whenMatched=replace whenNotMatched=discard' mode. Documents in the target
// collection without a match in the source collection should not be modified as a result
// of the merge operation.
(function testWhenMatchedReplaceWhenNotMatchedDiscard() {
    assert(target.drop());
    assert.commandWorked(target.insert([{_id: 1, a: 10}, {_id: 3, a: 30}, {_id: 4, a: 40}]));
    assert.doesNotThrow(() => source.aggregate([
        {$merge: {into: target.getName(), whenMatched: "replace", whenNotMatched: "discard"}}
    ]));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [{_id: 1, a: 1, b: "a"}, {_id: 3, a: 3, b: "c"}, {_id: 4, a: 40}]
    });
})();

// Test 'whenMatched=fail whenNotMatched=insert' mode. For matched documents the update should
// be unordered and report an error at the end when all documents in a batch have been
// processed, it will not fail as soon as we hit the first document with a match.
(function testWhenMatchedFailWhenNotMatchedInsert() {
    assert(target.drop());
    assert.commandWorked(target.insert(
        [{_id: 10, a: 10, c: "x"}, {_id: 3, a: 30, c: "y"}, {_id: 4, a: 40, c: "z"}]));
    // Besides ensuring that a DuplicateKey error is raised when we find a matching document,
    // this test also verifies that this $merge mode does perform an unordered insert and all
    // documents in the batch without a matching document get inserted into the target
    // collection. There is a special case when we can bail out early without processing all
    // documents which fit into a single batch. Namely, if we have a sharded cluster with two
    // shards, and shard documents by {_id: "hashed"}, we will end up with the document {_id: 3}
    // landed on shard0, and {_id: 1} and {_id: 2} on shard1 in the source collection. Note
    // that {_id: 3} has a duplicate key with the document in the target collection. For this
    // particlar case, the entire pipeline is sent to each shard. Lets assume that shard0 has
    // processed its single document with {_id: 3} and raised a DuplicateKey error, whilst
    // shard1 hasn't performed any writes yet (or even hasn't started reading from the cursor).
    // The mongos, after receiving the DuplicateKey, will stop pulling data from the shards
    // and will kill the cursors open on the remaining shards. Shard1, eventually, will throw
    // a CursorKilled during an interrupt check, and so no writes will be done into the target
    // collection. To workaround this scenario and guarantee that the writes will always be
    // performed, we will sort the documents by _id in ascending order. In this case, the
    // pipeline will be split and we will pull everything to mongos before doing the $merge.
    // This also ensures that documents with {_id: 1 } and {_id: 2} will be inserted first
    // before the DuplicateKey error is raised.
    const error = assert.throws(() => source.aggregate([
        {$sort: {_id: 1}},
        {$merge: {into: target.getName(), whenMatched: "fail", whenNotMatched: "insert"}}
    ]));
    assert.commandFailedWithCode(error, ErrorCodes.DuplicateKey);
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, a: 1, b: "a"},
            {_id: 2, a: 2, b: "b"},
            {_id: 3, a: 30, c: "y"},
            {_id: 4, a: 40, c: "z"},
            {_id: 10, a: 10, c: "x"}
        ]
    });
})();

// Test 'whenMatched=fail whenNotMatched=fail' mode. This mode is not supported and should fail.
(function testWhenMatchedFailWhenNotMatchedFail() {
    assert(target.drop());
    assert.commandWorked(target.insert({_id: 1, a: 10}));
    const error = assert.throws(
        () => source.aggregate(
            [{$merge: {into: target.getName(), whenMatched: "fail", whenNotMatched: "fail"}}]));
    assert.commandFailedWithCode(error, 51181);
    // Ensure the target collection has not been modified.
    assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, a: 10}]});
})();

// Test 'whenMatched=fail whenNotMatched=discard' mode. This mode is not supported and should
// fail.
(function testWhenMatchedFailWhenNotMatchedDiscard() {
    assert(target.drop());
    assert.commandWorked(target.insert({_id: 1, a: 10}));
    const error = assert.throws(
        () => source.aggregate(
            [{$merge: {into: target.getName(), whenMatched: "fail", whenNotMatched: "discard"}}]));
    assert.commandFailedWithCode(error, 51181);
    // Ensure the target collection has not been modified.
    assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, a: 10}]});
})();

// Test 'whenMatched=merge whenNotMatched=insert' mode. This is an equivalent of an update
// with a $set operator and upsert=true.
(function testWhenMatchedMergeWhenNotMatchedInsert() {
    assert(target.drop());
    assert.commandWorked(
        target.insert([{_id: 1, a: 10, c: "z"}, {_id: 3, a: 30}, {_id: 4, a: 40}]));
    assert.doesNotThrow(
        () => source.aggregate(
            [{$merge: {into: target.getName(), whenMatched: "merge", whenNotMatched: "insert"}}]));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, a: 1, c: "z", b: "a"},
            {_id: 2, a: 2, b: "b"},
            {_id: 3, a: 3, b: "c"},
            {_id: 4, a: 40}
        ]
    });
})();

// Test 'whenMatched=merge whenNotMatched=fail' mode. For matched documents the update
// should be unordered and report an error at the end when all documents in a batch have been
// processed, it will not fail as soon as we hit the first document without a match.
(function testWhenMatchedMergeWhenNotMatchedFail() {
    assert(target.drop());
    assert.commandWorked(
        target.insert([{_id: 1, a: 10, c: "x"}, {_id: 3, a: 30, c: "y"}, {_id: 4, a: 40, c: "z"}]));
    const error = assert.throws(
        () => source.aggregate(
            [{$merge: {into: target.getName(), whenMatched: "merge", whenNotMatched: "fail"}}]));
    assert.commandFailedWithCode(error, ErrorCodes.MergeStageNoMatchingDocument);
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, a: 1, b: "a", c: "x"},
            {_id: 3, a: 3, b: "c", c: "y"},
            {_id: 4, a: 40, c: "z"}
        ]
    });
})();

// Test 'whenMatched=merge whenNotMatched=discard' mode. Documents in the target collection
// without
// a match in the source collection should not be modified as a result of the merge operation.
(function testWhenMatchedMergeWhenNotMatchedDiscard() {
    assert(target.drop());
    assert.commandWorked(
        target.insert([{_id: 1, a: 10, c: "x"}, {_id: 3, a: 30, c: "y"}, {_id: 4, a: 40, c: "z"}]));
    assert.doesNotThrow(
        () => source.aggregate(
            [{$merge: {into: target.getName(), whenMatched: "merge", whenNotMatched: "discard"}}]));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, a: 1, b: "a", c: "x"},
            {_id: 3, a: 3, b: "c", c: "y"},
            {_id: 4, a: 40, c: "z"}
        ]
    });
})();

// Test 'whenMatched=[pipeline] whenNotMatched=insert' mode. This is an equivalent of a
// pipeline-style update with upsert=true and upsertSupplied=true.
(function testWhenMatchedPipelineUpdateWhenNotMatchedInsert() {
    assert(target.drop());
    assert.commandWorked(target.insert({_id: 1, b: 1}));
    assert.doesNotThrow(() => source.aggregate([{
        $merge:
            {into: target.getName(), whenMatched: [{$addFields: {x: 2}}], whenNotMatched: "insert"}
    }]));
    // We match {_id: 1} and apply the pipeline to add the field {x: 2}. The other source collection
    // documents are copied directly into the target collection.
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [{_id: 1, b: 1, x: 2}, {_id: 2, a: 2, b: "b"}, {_id: 3, a: 3, b: "c"}]
    });
})();

// Test 'whenMatched=[pipeline] whenNotMatched=fail' mode. For matched documents the update
// should be unordered and report an error at the end when all documents in a batch have been
// processed, it will not fail as soon as we hit the first document without a match.
(function testWhenMatchedPipelineUpdateWhenNotMatchedFail() {
    assert(target.drop());
    assert.commandWorked(
        target.insert([{_id: 1, a: 10, c: "x"}, {_id: 3, a: 30, c: "y"}, {_id: 4, a: 40, c: "z"}]));
    const error = assert.throws(() => source.aggregate([{
        $merge:
            {into: target.getName(), whenMatched: [{$addFields: {x: 2}}], whenNotMatched: "fail"}
    }]));
    assert.commandFailedWithCode(error, ErrorCodes.MergeStageNoMatchingDocument);
    assertArrayEq({
        actual: target.find().toArray(),
        expected:
            [{_id: 1, a: 10, c: "x", x: 2}, {_id: 3, a: 30, c: "y", x: 2}, {_id: 4, a: 40, c: "z"}]
    });
})();

// Test 'whenMatched=[pipeline] whenNotMatched=discard' mode. Documents in the target collection
// without a match in the source collection should not be modified as a result of the merge
// operation.
(function testWhenMatchedPipelineUpdateWhenNotMatchedDiscard() {
    assert(target.drop());
    assert.commandWorked(
        target.insert([{_id: 1, a: 10, c: "x"}, {_id: 3, a: 30, c: "y"}, {_id: 4, a: 40, c: "z"}]));
    assert.doesNotThrow(() => source.aggregate([{
        $merge:
            {into: target.getName(), whenMatched: [{$addFields: {x: 2}}], whenNotMatched: "discard"}
    }]));
    assertArrayEq({
        actual: target.find().toArray(),
        expected:
            [{_id: 1, a: 10, c: "x", x: 2}, {_id: 3, a: 30, c: "y", x: 2}, {_id: 4, a: 40, c: "z"}]
    });
})();

// Test 'whenMatched=keepExisting whenNotMatched=insert' mode. Existing documents in the target
// collection which have a matching document in the source collection must not be updated, only
// documents without a match must be inserted into the target collection.
(function testWhenMatchedKeepExistingWhenNotMatchedInsert() {
    assert(target.drop());
    assert.commandWorked(target.insert([{_id: 1, a: 10}, {_id: 3, a: 30}, {_id: 4, a: 40}]));
    assert.doesNotThrow(() => source.aggregate([
        {$merge: {into: target.getName(), whenMatched: "keepExisting", whenNotMatched: "insert"}}
    ]));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [{_id: 1, a: 10}, {_id: 2, a: 2, b: "b"}, {_id: 3, a: 30}, {_id: 4, a: 40}]
    });
})();

// Test 'whenMatched=keepExisting whenNotMatched=fail' mode. This mode is not supported and
// should fail.
(function testWhenMatchedKeepExistingWhenNotMatchedFail() {
    assert(target.drop());
    assert.commandWorked(target.insert({_id: 1, a: 10}));
    const error = assert.throws(() => source.aggregate([
        {$merge: {into: target.getName(), whenMatched: "keepExisting", whenNotMatched: "fail"}}
    ]));
    assert.commandFailedWithCode(error, 51181);
    // Ensure the target collection has not been modified.
    assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, a: 10}]});
})();

// Test 'whenMatched=keepExisting whenNotMatched=discard' mode. This mode is not supported and
// should fail.
(function testWhenMatchedKeepExistingWhenNotMatchedDiscard() {
    assert(target.drop());
    assert.commandWorked(target.insert({_id: 1, a: 10}));
    const error = assert.throws(() => source.aggregate([
        {$merge: {into: target.getName(), whenMatched: "keepExisting", whenNotMatched: "discard"}}
    ]));
    assert.commandFailedWithCode(error, 51181);
    // Ensure the target collection has not been modified.
    assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, a: 10}]});
})();
}());
