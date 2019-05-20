// Tests the behaviour of the $merge stage with whenMatched=[<pipeline>] and whenNotMatched=insert.
//
// Cannot implicitly shard accessed collections because a collection can be implictly created and
// exists when none is expected.
// @tags: [assumes_no_implicit_collection_creation_after_drop]
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
    load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers.isMongos.

    // A helper function to create a pipeline with a $merge stage using a custom 'updatePipeline'
    // for the whenMatched mode. If 'initialStages' array is specified, the $merge stage will be
    // appended to this array and the result returned to the caller, otherwise an array with a
    // single $merge stage is returned. An output collection for the $merge stage is specified
    // in the 'target', and the $merge stage 'on' fields in the 'on' parameter. The 'letVars'
    // parameter describes the 'let' argument of the $merge stage and holds variables that can be
    // referenced in the pipeline.
    function makeMergePipeline({target = "",
                                initialStages = [],
                                updatePipeline = [],
                                on = "_id",
                                letVars = undefined} = {}) {
        const baseObj = letVars !== undefined ? {let : letVars} : {};
        return initialStages.concat([{
            $merge: Object.assign(
                baseObj,
                {into: target, on: on, whenMatched: updatePipeline, whenNotMatched: "insert"})
        }]);
    }

    const source = db[`${jsTest.name()}_source`];
    source.drop();
    const target = db[`${jsTest.name()}_target`];
    target.drop();

    (function testMergeIntoNonExistentCollection() {
        assert.commandWorked(source.insert({_id: 1, a: 1, b: "a"}));
        assert.doesNotThrow(
            () => source.aggregate(makeMergePipeline(
                {target: target.getName(), updatePipeline: [{$addFields: {x: 1}}]})));
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [
                {_id: 1, x: 1},
            ]
        });
    })();

    // Test $merge inserts a document into an existing target collection if no matching document
    // is found.
    (function testMergeInsertsDocumentIfMatchNotFound() {
        assert.commandWorked(target.deleteMany({}));
        assert.doesNotThrow(
            () => source.aggregate(makeMergePipeline(
                {target: target.getName(), updatePipeline: [{$addFields: {x: 1, y: 2}}]})));
        assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, x: 1, y: 2}]});
    })();

    // Test $merge updates an existing document in the target collection by applying a
    // pipeline-style update.
    (function testMergeUpdatesDocumentIfMatchFound() {
        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            target: target.getName(),
            updatePipeline: [{$project: {x: {$add: ["$x", 1]}, y: {$add: ["$y", 2]}}}]
        })));
        assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, x: 2, y: 4}]});
    })();

    // Test $merge with various pipeline stages which are currently supported by the pipeline-style
    // update.
    (function testMergeWithSupportedUpdatePipelineStages() {
        assert(source.drop());
        assert(target.drop());

        assert.commandWorked(source.insert([{_id: 1, a: 1}, {_id: 2, a: 2}]));
        assert.commandWorked(target.insert({_id: 1, b: 1}));

        // Test $addFields stage.
        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            target: target.getName(),
            updatePipeline: [{$addFields: {x: {$add: ["$b", 1]}}}]
        })));
        assertArrayEq(
            {actual: target.find().toArray(), expected: [{_id: 1, b: 1, x: 2}, {_id: 2, x: null}]});

        // Test $project stage.
        assert(target.drop());
        assert.commandWorked(target.insert({_id: 1, b: 1}));
        assert.doesNotThrow(
            () => source.aggregate(makeMergePipeline(
                {target: target.getName(), updatePipeline: [{$project: {x: {$add: ["$b", 1]}}}]})));
        assertArrayEq(
            {actual: target.find().toArray(), expected: [{_id: 1, x: 2}, {_id: 2, x: null}]});

        // Test $replaceWith stage.
        assert(target.drop());
        assert.commandWorked(
            target.insert([{_id: 1, b: 1, c: {x: {y: 1}}}, {_id: 2, b: 2, c: {x: {y: 2}}}]));
        assert.doesNotThrow(
            () => source.aggregate(makeMergePipeline(
                {target: target.getName(), updatePipeline: [{$replaceWith: "$c"}]})));
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [{_id: 1, x: {y: 1}}, {_id: 2, x: {y: 2}}]
        });

        // Test $replaceRoot stage.
        assert(target.drop());
        assert.commandWorked(
            target.insert([{_id: 1, b: 1, c: {x: {y: 1}}}, {_id: 2, b: 2, c: {x: {y: 2}}}]));
        assert.doesNotThrow(
            () => source.aggregate(makeMergePipeline(
                {target: target.getName(), updatePipeline: [{$replaceRoot: {newRoot: "$c"}}]})));
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [{_id: 1, x: {y: 1}}, {_id: 2, x: {y: 2}}]
        });
    })();

    // Test $merge inserts a new document into the target collection if not matching document is
    // found by applying a pipeline-style update with upsert=true semantics.
    (function testMergeInsertDocumentIfMatchNotFound() {
        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(source.insert({_id: 1, a: 1}));
        assert.commandWorked(target.insert({_id: 2, a: 2}));
        assert.doesNotThrow(
            () => source.aggregate(makeMergePipeline(
                {target: target.getName(), updatePipeline: [{$addFields: {x: 1}}]})));
        assertArrayEq(
            {actual: target.find().toArray(), expected: [{_id: 1, x: 1}, {_id: 2, a: 2}]});
    })();

    // Test $merge doesn't modify the target collection if a document has been removed from the
    // source collection.
    (function testMergeDoesNotUpdateDeletedDocument() {
        assert.commandWorked(source.deleteOne({_id: 1}));
        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            target: target.getName(),
            updatePipeline: [{$project: {x: {$add: ["$x", 1]}, a: 1}}]
        })));
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [
                {_id: 1, x: 1},
                {_id: 2, a: 2},
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

        assert(target.drop());
        assert.commandWorked(source.insert({_id: 4, a: 2}));
        assert.commandWorked(target.insert([{_id: 1, x: 1}, {_id: 2, a: 2}]));
        assert.commandWorked(target.createIndex({a: 1}, {unique: true}));
        const error = assert.throws(
            () => source.aggregate(makeMergePipeline(
                {target: target.getName(), updatePipeline: [{$project: {x: 1, a: 1}}]})));
        assert.commandFailedWithCode(error, ErrorCodes.DuplicateKey);
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [
                {_id: 1, x: 1},
                {_id: 2, a: 2},
            ]
        });
        assert.commandWorked(target.dropIndex({a: 1}));
    })();

    // Test $merge fails if it cannot find an index to verify that the 'on' fields will be unique.
    (function testMergeFailsIfOnFieldCannotBeVerifiedForUniquness() {
        // The 'on' fields contains a single document field.
        let error = assert.throws(() => source.aggregate(makeMergePipeline({
            target: target.getName(),
            on: "nonexistent",
            updatePipeline: [{$project: {x: 1, a: 1}}]
        })));
        assert.commandFailedWithCode(error, [51190, 51183]);

        // The 'on' fields contains multiple document fields.
        error = assert.throws(() => source.aggregate(makeMergePipeline({
            target: target.getName(),
            on: ["nonexistent1", "nonexistent2"],
            updatePipeline: [{$project: {x: 1, a: 1}}]
        })));
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
        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(source.createIndex({a: 1}, {unique: true}));
        assert.commandWorked(target.createIndex({a: 1}, {unique: true}));
        assert.commandWorked(source.insert([{_id: 1, a: 1}, {_id: 2, a: 2}, {_id: 3, a: 30}]));
        assert.commandWorked(
            target.insert([{_id: 1, a: 1, b: 1}, {_id: 4, a: 30, b: 2}, {_id: 5, a: 40, b: 3}]));
        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            initialStages: [{$project: {_id: 0}}],
            target: target.getName(),
            on: "a",
            updatePipeline: [{$addFields: {z: 1}}]
        })));
        assertArrayEq({
            actual: target.find({}, {_id: 0}).toArray(),
            expected: [{a: 1, b: 1, z: 1}, {a: 2, z: 1}, {a: 30, b: 2, z: 1}, {a: 40, b: 3}]
        });

        // The 'on' fields contains multiple document fields.
        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(source.createIndex({a: 1, b: 1}, {unique: true}));
        assert.commandWorked(target.createIndex({a: 1, b: 1}, {unique: true}));
        assert.commandWorked(
            source.insert([{_id: 1, a: 1, b: 1}, {_id: 2, a: 2, b: 4}, {_id: 3, a: 30, b: 2}]));
        assert.commandWorked(
            target.insert([{_id: 1, a: 1, b: 1}, {_id: 4, a: 30, b: 2}, {_id: 5, a: 40, b: 3}]));
        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            initialStages: [{$project: {_id: 0}}],
            target: target.getName(),
            on: ["a", "b"],
            updatePipeline: [{$addFields: {z: 1}}]
        })));
        assertArrayEq({
            actual: target.find({}, {_id: 0}).toArray(),
            expected:
                [{a: 1, b: 1, z: 1}, {a: 2, b: 4, z: 1}, {a: 30, b: 2, z: 1}, {a: 40, b: 3}]
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

        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(source.createIndex({"a.b": 1}, {unique: true}));
        assert.commandWorked(target.createIndex({"a.b": 1}, {unique: true}));
        assert.commandWorked(source.insert([
            {_id: 1, a: {b: "b"}, c: "x"},
            {_id: 2, a: {b: "c"}, c: "y"},
            {_id: 3, a: {b: 30}, b: "c"}
        ]));
        assert.commandWorked(target.insert({_id: 2, a: {b: "c"}, c: "y"}));
        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            initialStages: [{$project: {_id: 0}}],
            target: target.getName(),
            on: "a.b",
            updatePipeline: [{$addFields: {z: 1}}]
        })));
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [
                {_id: 1, a: {b: "b"}, z: 1},
                {_id: 2, a: {b: "c"}, c: "y", z: 1},
                {_id: 3, a: {b: 30}, z: 1}
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

        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(source.createIndex({"z": 1}, {unique: true}));
        assert.commandWorked(target.createIndex({"z": 1}, {unique: true}));

        const pipeline = makeMergePipeline({
            initialStages: [{$project: {_id: 0}}],
            target: target.getName(),
            on: "z",
            updatePipeline: [{$addFields: {z: 1}}]
        });

        // The 'on' field is missing.
        assert.commandWorked(source.insert({_id: 1}));
        let error = assert.throws(() => source.aggregate(pipeline));
        assert.commandFailedWithCode(error, 51132);

        // The 'on' field is null.
        assert.commandWorked(source.update({_id: 1}, {z: null}));
        error = assert.throws(() => source.aggregate(pipeline));
        assert.commandFailedWithCode(error, 51132);

        // The 'on' field is an array.
        assert.commandWorked(source.update({_id: 1}, {z: [1, 2]}));
        error = assert.throws(() => source.aggregate(pipeline));
        assert.commandFailedWithCode(error, 51185);
    })();

    // Test $merge when the _id field is removed from the aggregate projection but is used in the
    // $merge's 'on' field. When the _id is missing, the $merge stage will create a new ObjectId in
    // its place before performing the insert or update.
    (function testMergeWhenDocIdIsRemovedFromProjection() {
        let pipeline = makeMergePipeline({
            initialStages: [{$project: {_id: 0}}],
            target: target.getName(),
            updatePipeline: [{$addFields: {z: 1}}]
        });

        // The _id is a single 'on' field (a default one).
        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(source.insert([{_id: 1, a: 1, b: "a"}, {_id: 2, a: 2, b: "b"}]));
        assert.commandWorked(target.insert({_id: 1, b: "c"}));
        assert.doesNotThrow(() => source.aggregate(pipeline));
        assertArrayEq({
            actual: target.find({}, {_id: 0}).toArray(),
            // There is a matching document in the target with {_id: 1}, but since we cannot match
            // it (no _id in projection), we just insert two new documents from the source
            // collection by applying a pipeline-style update.
            expected: [{b: "c"}, {z: 1}, {z: 1}]
        });

        pipeline = makeMergePipeline({
            initialStages: [{$project: {_id: 0}}],
            on: ["_id", "a"],
            target: target.getName(),
            updatePipeline: [{$addFields: {z: 1}}]
        });

        // The _id is part of the compound 'on' field.
        assert(target.drop());
        assert.commandWorked(target.insert({_id: 1, b: "c"}));
        assert.commandWorked(source.createIndex({_id: 1, a: -1}, {unique: true}));
        assert.commandWorked(target.createIndex({_id: 1, a: -1}, {unique: true}));
        assert.doesNotThrow(() => source.aggregate(pipeline));
        assertArrayEq({
            actual: target.find({}, {_id: 0}).toArray(),
            expected: [{b: "c"}, {a: 1, z: 1}, {a: 2, z: 1}]
        });
        assert.commandWorked(source.dropIndex({_id: 1, a: -1}));
        assert.commandWorked(target.dropIndex({_id: 1, a: -1}));
    })();

    // Test $merge preserves indexes and options of the existing target collection.
    (function testMergePresrvesIndexesAndOptions() {
        const validator = {z: {$gt: 0}};
        assert(target.drop());
        assert.commandWorked(db.createCollection(target.getName(), {validator: validator}));
        assert.commandWorked(target.createIndex({a: 1}));
        assert.doesNotThrow(
            () => source.aggregate(makeMergePipeline(
                {target: target.getName(), updatePipeline: [{$addFields: {z: 1}}]})));
        assertArrayEq(
            {actual: target.find().toArray(), expected: [{_id: 1, z: 1}, {_id: 2, z: 1}]});
        assert.eq(2, target.getIndexes().length);

        const listColl = db.runCommand({listCollections: 1, filter: {name: target.getName()}});
        assert.commandWorked(listColl);
        assert.eq(validator, listColl.cursor.firstBatch[0].options["validator"]);
    })();

    // Test $merge implicitly creates a new database when the target collection's database doesn't
    // exist.
    (function testMergeImplicitlyCreatesTargetDatabase() {
        assert(source.drop());
        assert.commandWorked(source.insert({_id: 1, a: 1, b: "a"}));

        const foreignDb = db.getSiblingDB(`${jsTest.name()}_foreign_db`);
        assert.commandWorked(foreignDb.dropDatabase());
        const foreignTarget = foreignDb[`${jsTest.name()}_target`];
        const foreignPipeline = makeMergePipeline({
            target: {db: foreignDb.getName(), coll: foreignTarget.getName()},
            updatePipeline: [{$addFields: {z: 1}}]
        });

        if (!FixtureHelpers.isMongos(db)) {
            assert.doesNotThrow(() => source.aggregate(foreignPipeline));
            assertArrayEq({actual: foreignTarget.find().toArray(), expected: [{_id: 1, z: 1}]});
        } else {
            // Implicit database creation is prohibited in a cluster.
            const error = assert.throws(() => source.aggregate(foreignPipeline));
            assert.commandFailedWithCode(error, ErrorCodes.NamespaceNotFound);

            // Force a creation of the database and collection, then fall through the test
            // below.
            assert.commandWorked(foreignTarget.insert({_id: 1}));
        }

        assert.doesNotThrow(() => source.aggregate(foreignPipeline));
        assertArrayEq({actual: foreignTarget.find().toArray(), expected: [{_id: 1, z: 1}]});
        assert.commandWorked(foreignDb.dropDatabase());
    })();

    // Test that $merge can reference the default 'let' variable 'new' which holds the entire
    // document from the source collection.
    (function testMergeWithDefaultLetVariable() {
        assert(source.drop());
        assert(target.drop());

        assert.commandWorked(source.insert([{_id: 1, a: 1, b: 1}, {_id: 2, a: 2, b: 2}]));
        assert.commandWorked(target.insert({_id: 1, c: 1}));

        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            target: target.getName(),
            updatePipeline: [{$set: {x: {$add: ["$$new.a", "$$new.b"]}}}]
        })));
        assertArrayEq(
            {actual: target.find().toArray(), expected: [{_id: 1, c: 1, x: 2}, {_id: 2, x: 4}]});
    })();

    // Test that the default 'let' variable 'new' is not available once the 'let' argument to the
    // $merge stage is specified explicitly.
    (function testMergeCannotUseDefaultLetVariableIfLetIsSpecified() {
        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(source.insert([{_id: 1, a: 1, b: 1}, {_id: 2, a: 2, b: 2}]));
        assert.commandWorked(target.insert({_id: 1, c: 1}));

        const error = assert.throws(() => source.aggregate(makeMergePipeline({
            letVars: {foo: "bar"},
            target: target.getName(),
            updatePipeline: [{$project: {x: "$$new.a", y: "$$new.b"}}]
        })));
        assert.commandFailedWithCode(error, 17276);
    })();

    // Test that $merge can accept an empty object holding no variables and the default 'new'
    // variable is not available.
    (function testMergeWithEmptyLetVariables() {
        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(source.insert([{_id: 1, a: 1, b: 1}, {_id: 2, a: 2, b: 2}]));
        assert.commandWorked(target.insert({_id: 1, c: 1}));

        // Can use an empty object.
        assert.doesNotThrow(
            () => source.aggregate(makeMergePipeline(
                {letVars: {}, target: target.getName(), updatePipeline: [{$set: {x: "foo"}}]})));
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [{_id: 1, c: 1, x: "foo"}, {_id: 2, x: "foo"}]
        });

        // No default variable 'new' is available.
        const error = assert.throws(() => source.aggregate(makeMergePipeline({
            letVars: {},
            target: target.getName(),
            updatePipeline: [{$project: {x: "$$new.a", y: "$$new.b"}}]
        })));
        assert.commandFailedWithCode(error, 17276);
    })();

    // Test that $merge can accept a null value as the 'let' argument and the default variable 'new'
    // can be used.
    // Note that this is not a desirable behaviour but rather a limitation in the IDL parser which
    // cannot differentiate between an optional field specified explicitly as 'null', or not
    // specified at all. In both cases it will treat the field like it wasn't specified. So, this
    // test ensures that we're aware of this limitation. Once the limitation is addressed in
    // SERVER-41272, this test should be updated to accordingly.
    (function testMergeWithNullLetVariables() {
        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(source.insert([{_id: 1, a: 1, b: 1}, {_id: 2, a: 2, b: 2}]));
        assert.commandWorked(target.insert({_id: 1, c: 1}));

        // Can use a null 'let' argument.
        assert.doesNotThrow(
            () => source.aggregate(makeMergePipeline(
                {letVars: null, target: target.getName(), updatePipeline: [{$set: {x: "foo"}}]})));
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [{_id: 1, c: 1, x: "foo"}, {_id: 2, x: "foo"}]
        });

        // Can use the default 'new' variable.
        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            letVars: null,
            target: target.getName(),
            updatePipeline: [{$project: {x: "$$new.a", y: "$$new.b"}}]
        })));
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [{_id: 1, x: 1, y: 1}, {_id: 2, x: 2, y: 2}]
        });
    })();

    // Test that constant values can be specified in the 'let' argument and referenced in the update
    // pipeline.
    (function testMergeWithConstantLetVariable() {
        // Non-array constants.
        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(source.insert([{_id: 1, a: 1, b: 1}, {_id: 2, a: 2, b: 2}]));
        assert.commandWorked(target.insert({_id: 1, c: 1}));

        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            letVars: {a: 1, b: "foo", c: true},
            target: target.getName(),
            updatePipeline: [{$set: {x: "$$a", y: "$$b", z: "$$c"}}]
        })));
        assertArrayEq({
            actual: target.find().toArray(),
            expected:
                [{_id: 1, c: 1, x: 1, y: "foo", z: true}, {_id: 2, x: 1, y: "foo", z: true}]
        });

        // Constant array.
        assert(target.drop());
        assert.commandWorked(target.insert({_id: 1, c: 1}));

        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            letVars: {a: [1, 2, 3]},
            target: target.getName(),
            updatePipeline: [{$set: {x: {$arrayElemAt: ["$$a", 1]}}}]
        })));
        assertArrayEq(
            {actual: target.find().toArray(), expected: [{_id: 1, c: 1, x: 2}, {_id: 2, x: 2}]});
    })();

    // Test that variables referencing the fields in the source document can be specified in the
    // 'let' argument and referenced in the update pipeline.
    (function testMergeWithNonConstantLetVariables() {
        // Non-array fields.
        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(source.insert([{_id: 1, a: 1, b: 1}, {_id: 2, a: 2, b: 2}]));
        assert.commandWorked(target.insert({_id: 1, c: 1}));

        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            letVars: {x: "$a", y: "$b"},
            target: target.getName(),
            updatePipeline: [{$set: {z: {$add: ["$$x", "$$y"]}}}]
        })));
        assertArrayEq(
            {actual: target.find().toArray(), expected: [{_id: 1, c: 1, z: 2}, {_id: 2, z: 4}]});

        // Array field with expressions in the pipeline.
        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(source.insert([{_id: 1, a: [1, 2, 3]}, {_id: 2, a: [4, 5, 6]}]));
        assert.commandWorked(target.insert({_id: 1, c: 1}));

        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            letVars: {x: "$a"},
            target: target.getName(),
            updatePipeline: [{$set: {z: {$arrayElemAt: ["$$x", 1]}}}]
        })));
        assertArrayEq(
            {actual: target.find().toArray(), expected: [{_id: 1, c: 1, z: 2}, {_id: 2, z: 5}]});

        // Array field with expressions in the 'let' argument.
        assert(target.drop());
        assert.commandWorked(target.insert({_id: 1, c: 1}));

        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            letVars: {x: {$arrayElemAt: ["$a", 2]}},
            target: target.getName(),
            updatePipeline: [{$set: {z: "$$x"}}]
        })));
        assertArrayEq(
            {actual: target.find().toArray(), expected: [{_id: 1, c: 1, z: 3}, {_id: 2, z: 6}]});
    })();

    // Test that variables using the dotted path can be specified in the 'let' argument and
    // referenced in the update pipeline.
    (function testMergeWithDottedPathLetVariables() {
        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(source.insert([{_id: 1, a: {b: {c: 2}}}, {_id: 2, a: {b: {c: 3}}}]));
        assert.commandWorked(target.insert({_id: 1, c: 1}));

        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            letVars: {x: "$a.b.c"},
            target: target.getName(),
            updatePipeline: [{$set: {z: {$pow: ["$$x", 2]}}}]
        })));
        assertArrayEq(
            {actual: target.find().toArray(), expected: [{_id: 1, c: 1, z: 4}, {_id: 2, z: 9}]});
    })();

    // Test that 'let' variables are referred to the computed document in the aggregation pipeline,
    // not the original document in the source collection.
    (function testMergeLetVariablesHoldsComputedValues() {
        // Test the default 'new' variable.
        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(
            source.insert([{_id: 1, a: 1, b: 1}, {_id: 2, a: 1, b: 2}, {_id: 3, a: 2, b: 3}]));
        assert.commandWorked(target.insert({_id: 1, c: 1}));

        // In the $group stage the total field 'a' uses the same name as in the source collection
        // intentionally, to make sure that even when a referenced field is present in the source
        // collection under the same name, the actual value for the variable will be picked up from
        // the computed document.
        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            initialStages: [{$group: {_id: "$a", a: {$sum: "$b"}}}],
            target: target.getName(),
            updatePipeline: [{$set: {z: "$$new"}}]
        })));
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [{_id: 1, c: 1, z: {_id: 1, a: 3}}, {_id: 2, z: {_id: 2, a: 3}}]
        });

        // Test custom 'let' variables.
        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(
            source.insert([{_id: 1, a: 1, b: 5}, {_id: 2, a: 1, b: 2}, {_id: 3, a: 2, b: 3}]));
        assert.commandWorked(target.insert({_id: 1, c: 1}));

        assert.doesNotThrow(() => source.aggregate(makeMergePipeline({
            initialStages: [{$group: {_id: "$a", a: {$sum: "$b"}}}],
            letVars: {x: {$pow: ["$a", 2]}},
            target: target.getName(),
            updatePipeline: [{$set: {z: "$$x"}}]
        })));
        assertArrayEq(
            {actual: target.find().toArray(), expected: [{_id: 1, c: 1, z: 49}, {_id: 2, z: 9}]});
    })();
}());
