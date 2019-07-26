// Tests the behaviour of the $merge stage with whenMatched=[pipeline] and whenNotMatched=discard.
//
// Cannot implicitly shard accessed collections because a collection can be implictly created and
// exists when none is expected.
// @tags: [assumes_no_implicit_collection_creation_after_drop]
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers.isSharded.

// A helper function to create a pipeline with a $merge stage using a custom 'updatePipeline'
// for the whenMatched mode. If 'initialStages' array is specified, the $merge stage will be
// appended to this array and the result returned to the caller, otherwise an array with a
// single $merge stage is returned. An output collection for the $merge stage is specified
// in the 'target', and the $merge stage 'on' fields in the 'on' parameter.
function makeMergePipeline(
    {target = "", initialStages = [], updatePipeline = [], on = "_id"} = {}) {
    return initialStages.concat(
        [{$merge: {into: target, on: on, whenMatched: updatePipeline, whenNotMatched: "discard"}}]);
}

const source = db[`${jsTest.name()}_source`];
source.drop();
const target = db[`${jsTest.name()}_target`];
target.drop();

// Test $merge when some documents in the source collection don't have a matching document in
// the target collection. The merge operation should succeed and unmatched documents discarded.
(function testMergeIfMatchingDocumentNotFound() {
    const pipeline =
        makeMergePipeline({target: target.getName(), updatePipeline: [{$set: {x: 1, y: 2}}]});

    // Single document without a match.
    assert.commandWorked(source.insert([{_id: 1, a: 1}, {_id: 2, a: 2}, {_id: 3, a: 3}]));
    assert.commandWorked(target.insert([{_id: 1, b: 1}, {_id: 3, b: 3}]));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [{_id: 1, b: 1, x: 1, y: 2}, {_id: 3, b: 3, x: 1, y: 2}]
    });

    // Multiple documents without a match.
    assert(target.drop());
    assert.commandWorked(target.insert([{_id: 1, b: 1}]));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, b: 1, x: 1, y: 2}]});
})();

// Test $merge when all documents in the source collection have a matching document in the
// target collection.
(function testMergeWhenAllDocumentsHaveMatch() {
    const pipeline =
        makeMergePipeline({target: target.getName(), updatePipeline: [{$set: {x: 1, y: 2}}]});

    // Source has a single element with a match in the target.
    assert(source.drop());
    assert(target.drop());
    assert.commandWorked(source.insert({_id: 3, a: 3}));
    assert.commandWorked(target.insert([{_id: 1, b: 1}, {_id: 3, b: 3}]));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq(
        {actual: target.find().toArray(), expected: [{_id: 1, b: 1}, {_id: 3, b: 3, x: 1, y: 2}]});

    // Source has multiple documents with matches in the target.
    assert(source.drop());
    assert(target.drop());
    assert.commandWorked(source.insert([{_id: 1, a: 1}, {_id: 2, a: 2}]));
    assert.commandWorked(target.insert([{_id: 1, b: 1}, {_id: 2, b: 2}, {_id: 3, b: 3}]));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [{_id: 1, b: 1, x: 1, y: 2}, {_id: 2, b: 2, x: 1, y: 2}, {_id: 3, b: 3}]
    });
})();

// Test $merge when the source collection is empty. The target collection should not be
// modified.
(function testMergeWhenSourceIsEmpty() {
    const pipeline =
        makeMergePipeline({target: target.getName(), updatePipeline: [{$set: {x: 1, y: 2}}]});

    assert.commandWorked(source.deleteMany({}));
    assert(target.drop());
    assert.commandWorked(target.insert([{_id: 1, b: 1}, {_id: 2, b: 2}]));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, b: 1}, {_id: 2, b: 2}]});
})();

// Test $merge does not insert a new document into the target collection if it was inserted
// into the source collection.
(function testMergeDoesNotInsertNewDocument() {
    const pipeline =
        makeMergePipeline({target: target.getName(), updatePipeline: [{$set: {x: 1, y: 2}}]});

    // Insert and merge a single document.
    assert.commandWorked(source.insert({_id: 3, a: 3, b: "c"}));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, b: 1}, {_id: 2, b: 2}]});
    assert.commandWorked(source.deleteOne({_id: 3}));

    // Insert and merge multiple documents.
    assert.commandWorked(source.insert({_id: 3, a: 3, b: "c"}));
    assert.commandWorked(source.insert({_id: 4, a: 4, c: "d"}));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, b: 1}, {_id: 2, b: 2}]});
    assert.commandWorked(source.deleteMany({_id: {$in: [3, 4]}}));
})();

// Test $merge doesn't modify the target collection if a document has been removed from the
// source collection.
(function testMergeDoesNotUpdateDeletedDocument() {
    const pipeline =
        makeMergePipeline({target: target.getName(), updatePipeline: [{$set: {x: 1, y: 2}}]});

    assert.commandWorked(source.deleteOne({_id: 1}));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, b: 1}, {_id: 2, b: 2}]});
})();

// Test $merge with an explicit 'on' field over a single or multiple document fields which
// differ from the _id field.
(function testMergeWithOnFields() {
    if (FixtureHelpers.isSharded(source)) {
        // Skip this test if the collection sharded, because an implicitly created sharded
        // key of {_id: 1} will not be covered by a unique index created in this test, which
        // is not allowed.
        return;
    }

    let pipeline = makeMergePipeline({
        initialStages: [{$project: {_id: 0}}],
        target: target.getName(),
        on: "a",
        updatePipeline: [{$set: {x: 1, y: 2}}]
    });

    // The 'on' fields contains a single document field.
    assert(source.drop());
    assert(target.drop());
    assert.commandWorked(source.createIndex({a: 1}, {unique: true}));
    assert.commandWorked(target.createIndex({a: 1}, {unique: true}));
    assert.commandWorked(
        source.insert([{_id: 1, a: 1, b: "a"}, {_id: 2, a: 2, b: "b"}, {_id: 3, a: 30, b: "c"}]));
    assert.commandWorked(
        target.insert([{_id: 1, a: 1, c: "x"}, {_id: 4, a: 30, c: "y"}, {_id: 5, a: 40, c: "z"}]));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, a: 1, c: "x", x: 1, y: 2},
            {_id: 4, a: 30, c: "y", x: 1, y: 2},
            {_id: 5, a: 40, c: "z"}
        ]
    });

    pipeline = makeMergePipeline({
        initialStages: [{$project: {_id: 0}}],
        target: target.getName(),
        on: ["a", "b"],
        updatePipeline: [{$set: {x: 1, y: 2}}]
    });

    // The 'on' fields contains multiple document fields.
    assert(source.drop());
    assert(target.drop());
    assert.commandWorked(source.createIndex({a: 1, b: 1}, {unique: true}));
    assert.commandWorked(target.createIndex({a: 1, b: 1}, {unique: true}));
    assert.commandWorked(source.insert(
        [{_id: 1, a: 1, b: "a", c: "x"}, {_id: 2, a: 2, b: "b"}, {_id: 3, a: 30, b: "c"}]));
    assert.commandWorked(target.insert(
        [{_id: 1, a: 1, b: "a"}, {_id: 4, a: 30, b: "c", c: "y"}, {_id: 5, a: 40, c: "z"}]));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, a: 1, b: "a", x: 1, y: 2},
            {_id: 4, a: 30, b: "c", c: "y", x: 1, y: 2},
            {_id: 5, a: 40, c: "z"}
        ]
    });
    assert.commandWorked(source.dropIndex({a: 1, b: 1}));
    assert.commandWorked(target.dropIndex({a: 1, b: 1}));
})();

// Test $merge with a dotted path in the 'on' field.
(function testMergeWithDottedOnField() {
    if (FixtureHelpers.isSharded(source)) {
        // Skip this test if the collection sharded, because an implicitly created sharded
        // key of {_id: 1} will not be covered by a unique index created in this test, which
        // is not allowed.
        return;
    }

    const pipeline = makeMergePipeline({
        initialStages: [{$project: {_id: 0}}],
        target: target.getName(),
        on: "a.b",
        updatePipeline: [{$set: {x: 1, y: 2}}]
    });

    assert(source.drop());
    assert(target.drop());
    assert.commandWorked(source.createIndex({"a.b": 1}, {unique: true}));
    assert.commandWorked(target.createIndex({"a.b": 1}, {unique: true}));
    assert.commandWorked(source.insert([
        {_id: 1, a: {b: "b"}, c: "x"},
        {_id: 2, a: {b: "c"}, c: "y"},
        {_id: 3, a: {b: 30}, b: "c"}
    ]));
    assert.commandWorked(target.insert({_id: 2, a: {b: "c"}}));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 2, a: {b: "c"}, x: 1, y: 2},
        ]
    });
})();

// Test $merge when the _id field is removed from the aggregate projection but is used in the
// $merge's 'on' field.
(function testMergeWhenDocIdIsRemovedFromProjection() {
    let pipeline = makeMergePipeline({
        initialStages: [{$project: {_id: 0}}],
        target: target.getName(),
        updatePipeline: [{$set: {x: 1, y: 2}}]
    });

    // The _id is a single 'on' field (a default one).
    assert(source.drop());
    assert(target.drop());
    assert.commandWorked(source.insert([{_id: 1, a: 1, b: "a"}, {_id: 2, a: 2, b: "b"}]));
    assert.commandWorked(target.insert({_id: 1, b: "c"}));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({actual: target.find({}, {_id: 0}).toArray(), expected: [{b: "c"}]});

    pipeline = makeMergePipeline({
        initialStages: [{$project: {_id: 0}}],
        on: ["_id", "a"],
        target: target.getName(),
        updatePipeline: [{$set: {x: 1, y: 2}}]
    });

    // The _id is part of the compound 'on' field.
    assert(target.drop());
    assert.commandWorked(target.insert({_id: 1, b: "c"}));
    assert.commandWorked(source.createIndex({_id: 1, a: -1}, {unique: true}));
    assert.commandWorked(target.createIndex({_id: 1, a: -1}, {unique: true}));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({actual: target.find({}, {_id: 0}).toArray(), expected: [{b: "c"}]});
    assert.commandWorked(source.dropIndex({_id: 1, a: -1}));
    assert.commandWorked(target.dropIndex({_id: 1, a: -1}));
})();

// Test that variables referencing the fields in the source document can be specified in the
// 'let' argument and referenced in the update pipeline.
(function testMergeWithLetVariables() {
    assert(source.drop());
    assert(target.drop());
    assert.commandWorked(source.insert([{_id: 1, a: 1, b: 1}, {_id: 2, a: 2, b: 2}]));
    assert.commandWorked(target.insert([{_id: 1, c: 1}]));

    assert.doesNotThrow(() => source.aggregate([{
        $merge: {
            into: target.getName(),
            let : {x: "$a", y: "$b"},
            whenMatched: [{$set: {z: {$add: ["$$x", "$$y"]}}}],
            whenNotMatched: "discard"
        }
    }]));
    assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, c: 1, z: 2}]});
})();
}());
