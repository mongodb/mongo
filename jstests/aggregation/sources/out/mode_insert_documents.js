// Tests the behavior of $out with mode "insertDocuments".
// @tags: [assumes_unsharded_collection, assumes_no_implicit_collection_creation_after_drop]
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

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
    assertErrorCode(coll, pipeline, 16996);

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

    assertErrorCode(coll, pipeline, 16996);

}());
