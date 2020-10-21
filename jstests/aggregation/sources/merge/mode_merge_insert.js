// Tests the behaviour of the $merge stage with whenMatched=merge and whenNotMatched=insert.
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
    $merge: {into: target.getName(), whenMatched: "merge", whenNotMatched: "insert"}
};
const pipeline = [mergeStage];

// Test $merge into a non-existent collection.
(function testMergeIntoNonExistentCollection() {
    assert.commandWorked(source.insert({_id: 1, a: 1, b: "a"}));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, a: 1, b: "a"},
        ]
    });
})();

// Test $merge into an existing collection.
(function testMergeIntoExistentCollection() {
    assert.commandWorked(source.insert({_id: 2, a: 2, b: "b"}));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [{_id: 1, a: 1, b: "a"}, {_id: 2, a: 2, b: "b"}]
    });
})();

// Test $merge does not update documents in the target collection if they were not modified
// in the source collection.
(function testMergeDoesNotUpdateUnmodifiedDocuments() {
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [{_id: 1, a: 1, b: "a"}, {_id: 2, a: 2, b: "b"}]
    });
})();

// Test $merge updates documents in the target collection if they were modified in the source
// collection.
(function testMergeUpdatesModifiedDocuments() {
    // Update and merge a single document.
    assert.commandWorked(source.update({_id: 2}, {a: 22, c: "c"}));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [{_id: 1, a: 1, b: "a"}, {_id: 2, a: 22, b: "b", c: "c"}]
    });

    // Update and merge multiple documents.
    assert.commandWorked(source.update({_id: 1}, {a: 11}));
    assert.commandWorked(source.update({_id: 2}, {a: 22, c: "c", d: "d"}));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [{_id: 1, a: 11, b: "a"}, {_id: 2, a: 22, b: "b", c: "c", d: "d"}]
    });
})();

// Test $merge inserts a new document into the target collection if it was inserted into the
// source collection.
(function testMergeInsertsNewDocument() {
    // Insert and merge a single document.
    assert.commandWorked(source.insert({_id: 3, a: 3, b: "c"}));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, a: 11, b: "a"},
            {_id: 2, a: 22, b: "b", c: "c", d: "d"},
            {_id: 3, a: 3, b: "c"}
        ]
    });
    assert.commandWorked(source.deleteOne({_id: 3}));
    assert.commandWorked(target.deleteOne({_id: 3}));

    // Insert and merge multiple documents.
    assert.commandWorked(source.insert({_id: 3, a: 3, b: "c"}));
    assert.commandWorked(source.insert({_id: 4, a: 4, c: "d"}));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, a: 11, b: "a"},
            {_id: 2, a: 22, b: "b", c: "c", d: "d"},
            {_id: 3, a: 3, b: "c"},
            {_id: 4, a: 4, c: "d"}
        ]
    });
    assert.commandWorked(source.deleteMany({_id: {$in: [3, 4]}}));
    assert.commandWorked(target.deleteMany({_id: {$in: [3, 4]}}));
})();

// Test $merge doesn't modify the target collection if a document has been removed from the
// source collection.
(function testMergeDoesNotUpdateDeletedDocument() {
    assert.commandWorked(source.deleteOne({_id: 1}));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, a: 11, b: "a"},
            {_id: 2, a: 22, b: "b", c: "c", d: "d"},
        ]
    });
})();

// Test $merge fails if a unique index constraint in the target collection is violated.
(function testMergeFailsIfTargetUniqueKeyIsViolated() {
    if (FixtureHelpers.isSharded(source)) {
        // Skip this test if the collection sharded, because an implicitly created sharded
        // key of {_id: 1} will not be covered by a unique index created in this test, which
        // is not allowed.
        return;
    }
    dropWithoutImplicitRecreate(source.getName());

    assert.commandWorked(source.insert({_id: 4, a: 11}));
    assert.commandWorked(target.createIndex({a: 1}, {unique: true}));
    const error = assert.throws(() => source.aggregate(pipeline));
    assert.commandFailedWithCode(error, ErrorCodes.DuplicateKey);
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, a: 11, b: "a"},
            {_id: 2, a: 22, b: "b", c: "c", d: "d"},
        ]
    });
    assert.commandWorked(target.dropIndex({a: 1}));
})();

// Test $merge fails if it cannot find an index to verify that the 'on' fields will be unique.
(function testMergeFailsIfOnFieldCannotBeVerifiedForUniquness() {
    // The 'on' fields contains a single document field.
    let error = assert.throws(
        () => source.aggregate([{$merge: Object.assign({on: "nonexistent"}, mergeStage.$merge)}]));
    assert.commandFailedWithCode(error, [51190, 51183]);

    // The 'on' fields contains multiple document fields.
    error = assert.throws(
        () => source.aggregate(
            [{$merge: Object.assign({on: ["nonexistent1", "nonexistent2"]}, mergeStage.$merge)}]));
    assert.commandFailedWithCode(error, [51190, 51183]);
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

    // The 'on' fields contains a single document field.
    dropWithoutImplicitRecreate(source.getName());
    dropWithoutImplicitRecreate(target.getName());
    assert.commandWorked(source.createIndex({a: 1}, {unique: true}));
    assert.commandWorked(target.createIndex({a: 1}, {unique: true}));
    assert.commandWorked(
        source.insert([{_id: 1, a: 1, b: "a"}, {_id: 2, a: 2, b: "b"}, {_id: 3, a: 30, b: "c"}]));
    assert.commandWorked(
        target.insert([{_id: 1, a: 1, c: "x"}, {_id: 4, a: 30, c: "y"}, {_id: 5, a: 40, c: "z"}]));
    assert.doesNotThrow(
        () => source.aggregate(
            [{$project: {_id: 0}}, {$merge: Object.assign({on: "a"}, mergeStage.$merge)}]));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, a: 1, b: "a", c: "x"},
            {_id: 2, a: 2, b: "b"},
            {_id: 4, a: 30, b: "c", c: "y"},
            {_id: 5, a: 40, c: "z"}
        ]
    });

    // The 'on' fields contains multiple document fields.
    dropWithoutImplicitRecreate(source.getName());
    dropWithoutImplicitRecreate(target.getName());
    assert.commandWorked(source.createIndex({a: 1, b: 1}, {unique: true}));
    assert.commandWorked(target.createIndex({a: 1, b: 1}, {unique: true}));
    assert.commandWorked(source.insert(
        [{_id: 1, a: 1, b: "a", c: "x"}, {_id: 2, a: 2, b: "b"}, {_id: 3, a: 30, b: "c"}]));
    assert.commandWorked(target.insert(
        [{_id: 1, a: 1, b: "a"}, {_id: 4, a: 30, b: "c", c: "y"}, {_id: 5, a: 40, c: "z"}]));
    assert.doesNotThrow(
        () => source.aggregate(
            [{$project: {_id: 0}}, {$merge: Object.assign({on: ["a", "b"]}, mergeStage.$merge)}]));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, a: 1, b: "a", c: "x"},
            {_id: 2, a: 2, b: "b"},
            {_id: 4, a: 30, b: "c", c: "y"},
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
    dropWithoutImplicitRecreate(source.getName());
    dropWithoutImplicitRecreate(target.getName());

    assert.commandWorked(source.createIndex({"a.b": 1}, {unique: true}));
    assert.commandWorked(target.createIndex({"a.b": 1}, {unique: true}));
    assert.commandWorked(source.insert([
        {_id: 1, a: {b: "b"}, c: "x"},
        {_id: 2, a: {b: "c"}, c: "y"},
        {_id: 3, a: {b: 30}, b: "c"}
    ]));
    assert.commandWorked(target.insert({_id: 2, a: {b: "c"}}));
    assert.doesNotThrow(
        () => source.aggregate(
            [{$project: {_id: 0}}, {$merge: Object.assign({on: "a.b"}, mergeStage.$merge)}]));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, a: {b: "b"}, c: "x"},
            {_id: 2, a: {b: "c"}, c: "y"},
            {_id: 3, a: {b: 30}, b: "c"}
        ]
    });
})();

// Test $merge fails if the value of the 'on' field in a document is invalid, e.g. missing,
// null or an array.
(function testMergeFailsIfOnFieldIsInvalid() {
    if (FixtureHelpers.isSharded(source)) {
        // Skip this test if the collection sharded, because an implicitly created sharded
        // key of {_id: 1} will not be covered by a unique index created in this test, which
        // is not allowed.
        return;
    }
    dropWithoutImplicitRecreate(source.getName());
    dropWithoutImplicitRecreate(target.getName());

    assert.commandWorked(source.createIndex({"z": 1}, {unique: true}));
    assert.commandWorked(target.createIndex({"z": 1}, {unique: true}));

    // The 'on' field is missing.
    assert.commandWorked(source.insert({_id: 1}));
    let error = assert.throws(
        () => source.aggregate(
            [{$project: {_id: 0}}, {$merge: Object.assign({on: "z"}, mergeStage.$merge)}]));
    assert.commandFailedWithCode(error, 51132);

    // The 'on' field is null.
    assert.commandWorked(source.update({_id: 1}, {z: null}));
    error = assert.throws(
        () => source.aggregate(
            [{$project: {_id: 0}}, {$merge: Object.assign({on: "z"}, mergeStage.$merge)}]));
    assert.commandFailedWithCode(error, 51132);

    // The 'on' field is an array.
    assert.commandWorked(source.update({_id: 1}, {z: [1, 2]}));
    error = assert.throws(
        () => source.aggregate(
            [{$project: {_id: 0}}, {$merge: Object.assign({on: "z"}, mergeStage.$merge)}]));
    assert.commandFailedWithCode(error, 51185);
})();

// Test $merge when the _id field is removed from the aggregate projection but is used in the
// $merge's 'on' field.
(function testMergeWhenDocIdIsRemovedFromProjection() {
    // The _id is a single 'on' field (a default one).
    dropWithoutImplicitRecreate(source.getName());
    assert(target.drop());
    assert.commandWorked(source.insert([{_id: 1, a: 1, b: "a"}, {_id: 2, a: 2, b: "b"}]));
    assert.commandWorked(target.insert({_id: 1, b: "c"}));
    assert.doesNotThrow(() => source.aggregate([{$project: {_id: 0}}, mergeStage]));
    assertArrayEq({
        actual: target.find({}, {_id: 0}).toArray(),
        expected: [{b: "c"}, {a: 1, b: "a"}, {a: 2, b: "b"}]
    });

    // The _id is part of the compound 'on' field.
    dropWithoutImplicitRecreate(target.getName());
    assert.commandWorked(target.insert({_id: 1, b: "c"}));
    assert.commandWorked(source.createIndex({_id: 1, a: -1}, {unique: true}));
    assert.commandWorked(target.createIndex({_id: 1, a: -1}, {unique: true}));
    assert.doesNotThrow(() => source.aggregate([
        {$project: {_id: 0}},
        {$merge: Object.assign({on: ["_id", "a"]}, mergeStage.$merge)}
    ]));
    assertArrayEq({
        actual: target.find({}, {_id: 0}).toArray(),
        expected: [{b: "c"}, {a: 1, b: "a"}, {a: 2, b: "b"}]
    });
    assert.commandWorked(source.dropIndex({_id: 1, a: -1}));
    assert.commandWorked(target.dropIndex({_id: 1, a: -1}));
})();

// Test $merge preserves indexes and options of the existing target collection.
(function testMergePresrvesIndexesAndOptions() {
    const validator = {a: {$gt: 0}};
    dropWithoutImplicitRecreate(target.getName());
    assert.commandWorked(db.createCollection(target.getName(), {validator: validator}));
    assert.commandWorked(target.createIndex({a: 1}));
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [{_id: 1, a: 1, b: "a"}, {_id: 2, a: 2, b: "b"}]
    });
    assert.eq(2, target.getIndexes().length);

    const listColl = db.runCommand({listCollections: 1, filter: {name: target.getName()}});
    assert.commandWorked(listColl);
    assert.eq(validator, listColl.cursor.firstBatch[0].options["validator"]);
})();

// Test $merge implicitly creates a new database when the target collection's database doesn't
// exist.
(function testMergeImplicitlyCreatesTargetDatabase() {
    dropWithoutImplicitRecreate(source.getName());
    assert.commandWorked(source.insert({_id: 1, a: 1, b: "a"}));

    const foreignDb = db.getSiblingDB(`${jsTest.name()}_foreign_db`);
    assert.commandWorked(foreignDb.dropDatabase());
    const foreignTargetCollName = jsTest.name() + "_target";
    const foreignPipeline = [{
        $merge: {
            into: {db: foreignDb.getName(), coll: foreignTargetCollName},
            whenMatched: "merge",
            whenNotMatched: "insert"
        }
    }];

    if (!FixtureHelpers.isMongos(db)) {
        assert.doesNotThrow(() => source.aggregate(foreignPipeline));
        assertArrayEq({
            actual: foreignDb[foreignTargetCollName].find().toArray(),
            expected: [{_id: 1, a: 1, b: "a"}]
        });
    } else {
        // Implicit database creation is prohibited in a cluster.
        const error = assert.throws(() => source.aggregate(foreignPipeline));
        assert.commandFailedWithCode(error, ErrorCodes.NamespaceNotFound);

        // Force a creation of the database and collection, then fall through the test below.
        assert.commandWorked(foreignDb[foreignTargetCollName].insert({_id: 1, a: 1}));
    }

    assert.doesNotThrow(() => source.aggregate(foreignPipeline));
    assertArrayEq({
        actual: foreignDb[foreignTargetCollName].find().toArray(),
        expected: [{_id: 1, a: 1, b: "a"}]
    });
    assert.commandWorked(foreignDb.dropDatabase());
})();
}());
