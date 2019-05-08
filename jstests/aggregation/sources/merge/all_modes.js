// Tests basic use cases for all $merge modes.
//
// Cannot implicitly shard accessed collections because a collection can be implictly created and
// exists when none is expected.
// @tags: [assumes_no_implicit_collection_creation_after_drop]
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.

    const source = db.all_modes_source;
    const target = db.all_modes_target;

    (function setup() {
        source.drop();
        target.drop();

        // All tests use the same data in the source collection.
        assert.commandWorked(source.insert(
            [{_id: 1, a: 1, b: "a"}, {_id: 2, a: 2, b: "b"}, {_id: 3, a: 3, b: "c"}]));

    })();

    // Test 'whenMatched=replaceWithNew whenNotMatched=insert' mode. This is an equivalent of a
    // replacement-style update with upsert=true.
    (function testWhenMatchedReplaceWithNewWhenNotMatchedInsert() {
        assert.commandWorked(target.insert([{_id: 1, a: 10}, {_id: 3, a: 30}, {_id: 4, a: 40}]));
        assert.doesNotThrow(() => source.aggregate([{
            $merge: {
                into: target.getName(),
                whenMatched: "replaceWithNew",
                whenNotMatched: "insert"
            }
        }]));
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

    // Test 'whenMatched=replaceWithNew whenNotMatched=fail' mode. For matched documents the update
    // should be unordered and report an error at the end when all documents in a batch have been
    // processed, it will not fail as soon as we hit the first document without a match.
    (function testWhenMatchedReplaceWithNewWhenNotMatchedFail() {
        assert(target.drop());
        assert.commandWorked(target.insert([{_id: 1, a: 10}, {_id: 3, a: 30}, {_id: 4, a: 40}]));
        const error = assert.throws(() => source.aggregate([{
            $merge:
                {into: target.getName(), whenMatched: "replaceWithNew", whenNotMatched: "fail"}
        }]));
        assert.commandFailedWithCode(error, ErrorCodes.MergeStageNoMatchingDocument);
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
        const error = assert.throws(() => source.aggregate([
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

    // Test 'whenMatched=merge whenNotMatched=insert' mode. This is an equivalent of an update
    // with a $set operator and upsert=true.
    (function testWhenMatchedMergeWhenNotMatchedInsert() {
        assert(target.drop());
        assert.commandWorked(
            target.insert([{_id: 1, a: 10, c: "z"}, {_id: 3, a: 30}, {_id: 4, a: 40}]));
        assert.doesNotThrow(() => source.aggregate([
            {$merge: {into: target.getName(), whenMatched: "merge", whenNotMatched: "insert"}}
        ]));
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
        assert.commandWorked(target.insert(
            [{_id: 1, a: 10, c: "x"}, {_id: 3, a: 30, c: "y"}, {_id: 4, a: 40, c: "z"}]));
        const error = assert.throws(() => source.aggregate([
            {$merge: {into: target.getName(), whenMatched: "merge", whenNotMatched: "fail"}}
        ]));
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

    // Test 'whenMatched=[pipeline] whenNotMatched=insert' mode. This is an equivalent of a
    // pipeline-style update with upsert=true.
    (function testWhenMatchedPipelineUpdateWhenNotMatchedInsert() {
        assert(target.drop());
        assert.commandWorked(target.insert({_id: 1, b: 1}));
        assert.doesNotThrow(() => source.aggregate([{
            $merge: {
                into: target.getName(),
                whenMatched: [{$addFields: {x: 2}}],
                whenNotMatched: "insert"
            }
        }]));
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [{_id: 1, b: 1, x: 2}, {_id: 2, x: 2}, {_id: 3, x: 2}]
        });
    })();

    // Test 'whenMatched=[pipeline] whenNotMatched=fail' mode. For matched documents the update
    // should be unordered and report an error at the end when all documents in a batch have been
    // processed, it will not fail as soon as we hit the first document without a match.
    (function testWhenMatchedPipelineUpdateWhenNotMatchedFail() {
        assert(target.drop());
        assert.commandWorked(target.insert(
            [{_id: 1, a: 10, c: "x"}, {_id: 3, a: 30, c: "y"}, {_id: 4, a: 40, c: "z"}]));
        const error = assert.throws(() => source.aggregate([{
            $merge: {
                into: target.getName(),
                whenMatched: [{$addFields: {x: 2}}],
                whenNotMatched: "fail"
            }
        }]));
        assert.commandFailedWithCode(error, ErrorCodes.MergeStageNoMatchingDocument);
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [
                {_id: 1, a: 10, c: "x", x: 2},
                {_id: 3, a: 30, c: "y", x: 2},
                {_id: 4, a: 40, c: "z"}
            ]
        });
    })();

    // Test 'whenMatched=keepExisting whenNotMatched=insert' mode. Existing documents in the target
    // collection which have a matching document in the source collection must not be updated, only
    // documents without a match must be inserted into the target collection.
    (function testWhenMatchedKeepExistingWhenNotMatchedInsert() {
        assert(target.drop());
        assert.commandWorked(target.insert([{_id: 1, a: 10}, {_id: 3, a: 30}, {_id: 4, a: 40}]));
        assert.doesNotThrow(() => source.aggregate([{
            $merge:
                {into: target.getName(), whenMatched: "keepExisting", whenNotMatched: "insert"}
        }]));
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
        const error = assert.throws(() => source.aggregate([{
            $merge:
                {into: target.getName(), whenMatched: "keepExisting", whenNotMatched: "fail"}
        }]));
        assert.commandFailedWithCode(error, 51181);
        // Ensure the target collection has not been modified.
        assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, a: 10}]});
    })();
}());
