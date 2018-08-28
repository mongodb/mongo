// Tests the behavior of $out with mode "insertDocuments".
// @tags: [assumes_unsharded_collection, assumes_no_implicit_collection_creation_after_drop]
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.
    load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers.isMongos.

    const coll = db.mode_insert_documents;
    coll.drop();

    const targetColl = db.mode_insert_documents_out;
    targetColl.drop();

    const pipeline = [{$out: {to: targetColl.getName(), mode: "insertDocuments"}}];

    //
    // Test $out with a non-existent output collection.
    //
    assert.commandWorked(coll.insert({_id: 0}));

    coll.aggregate(pipeline);
    assert.eq(1, targetColl.find().itcount());

    //
    // Test $out with an existing output collection.
    //
    assert.commandWorked(coll.remove({_id: 0}));
    assert.commandWorked(coll.insert({_id: 1}));
    coll.aggregate(pipeline);
    assert.eq(2, targetColl.find().itcount());

    //
    // Test that $out fails if there's a duplicate key error.
    //
    assertErrorCode(coll, pipeline, ErrorCodes.DuplicateKey);

    //
    // Test that $out will preserve the indexes and options of the output collection.
    //
    const validator = {a: {$gt: 0}};
    targetColl.drop();
    assert.commandWorked(db.createCollection(targetColl.getName(), {validator: validator}));
    assert.commandWorked(targetColl.createIndex({a: 1}));

    coll.drop();
    assert.commandWorked(coll.insert({a: 1}));

    coll.aggregate(pipeline);
    assert.eq(1, targetColl.find().itcount());
    assert.eq(2, targetColl.getIndexes().length);

    const listColl = db.runCommand({listCollections: 1, filter: {name: targetColl.getName()}});
    assert.commandWorked(listColl);
    assert.eq(validator, listColl.cursor.firstBatch[0].options["validator"]);

    //
    // Test that $out fails if it violates a unique index constraint.
    //
    coll.drop();
    assert.commandWorked(coll.insert([{_id: 0, a: 0}, {_id: 1, a: 0}]));
    targetColl.drop();
    assert.commandWorked(targetColl.createIndex({a: 1}, {unique: true}));

    assertErrorCode(coll, pipeline, ErrorCodes.DuplicateKey);

    //
    // Tests for $out to a database that differs from the aggregation database.
    //
    const foreignDb = db.getSiblingDB("mode_insert_documents_foreign");
    const foreignTargetColl = foreignDb.mode_insert_documents_out;
    const pipelineDifferentOutputDb = [{
        $out: {
            to: foreignTargetColl.getName(),
            db: foreignDb.getName(),
            mode: "insertDocuments",
        }
    }];

    foreignDb.dropDatabase();

    if (!FixtureHelpers.isMongos(db)) {
        //
        // Test that $out implicitly creates a new database when the output collection's database
        // doesn't exist.
        //
        coll.aggregate(pipelineDifferentOutputDb);
        assert.eq(foreignTargetColl.find().itcount(), 2);

        //
        // First, replace the contents of the collection with new documents that have different _id
        // values. Then, re-run the same aggregation, which should merge the new documents into the
        // existing output collection without overwriting the existing, non-conflicting documents.
        //
        coll.drop();
        assert.commandWorked(coll.insert([{_id: 2, a: 2}, {_id: 3, a: 3}]));
        coll.aggregate(pipelineDifferentOutputDb);
        assert.eq(foreignTargetColl.find().itcount(), 4);
    } else {
        // Implicit database creation is prohibited in a cluster.
        let error = assert.throws(() => coll.aggregate(pipelineDifferentOutputDb));
        assert.commandFailedWithCode(error, ErrorCodes.NamespaceNotFound);
    }
}());
