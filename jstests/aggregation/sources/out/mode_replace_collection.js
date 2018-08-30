/**
 * Tests the behavior of $out with mode "replaceCollection".
 *
 * This test assumes that collections are not implicitly sharded, since mode "replaceCollection" is
 * prohibited if the output collection is sharded.
 * @tags: [assumes_unsharded_collection]
 */
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.
    load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers.isMongos.

    const coll = db.mode_replace_collection;
    coll.drop();

    const targetColl = db.mode_replace_collection_out;
    targetColl.drop();

    const pipeline = [{$out: {to: targetColl.getName(), mode: "replaceCollection"}}];

    //
    // Test $out with a non-existent output collection.
    //
    assert.commandWorked(coll.insert({_id: 0}));
    coll.aggregate(pipeline);
    assert.eq(1, targetColl.find().itcount());

    //
    // Test $out with an existing output collection.
    //
    coll.aggregate(pipeline);
    assert.eq(1, targetColl.find().itcount());

    //
    // Test that $out will preserve the indexes and options of the output collection.
    //
    targetColl.drop();
    assert.commandWorked(db.createCollection(targetColl.getName(), {validator: {a: {$gt: 0}}}));
    assert.commandWorked(targetColl.createIndex({a: 1}));

    coll.drop();
    assert.commandWorked(coll.insert({a: 1}));

    coll.aggregate(pipeline);
    assert.eq(1, targetColl.find().itcount());
    assert.eq(2, targetColl.getIndexes().length);

    const listColl = db.runCommand({listCollections: 1, filter: {name: targetColl.getName()}});
    assert.commandWorked(listColl);
    assert.eq({a: {$gt: 0}}, listColl.cursor.firstBatch[0].options["validator"]);

    //
    // Test that $out fails if it violates a unique index constraint.
    //
    coll.drop();
    assert.commandWorked(coll.insert([{_id: 0, a: 0}, {_id: 1, a: 0}]));
    targetColl.drop();
    assert.commandWorked(targetColl.createIndex({a: 1}, {unique: true}));

    assertErrorCode(coll, pipeline, ErrorCodes.DuplicateKey);

    // Rerun a similar test, except populate the target collection with a document that conflics
    // with one out of the pipeline. In this case, there is no unique key violation since the target
    // collection will be dropped before renaming the source collection.
    coll.drop();
    assert.commandWorked(coll.insert({_id: 0, a: 0}));
    targetColl.remove({});
    assert.commandWorked(targetColl.insert({_id: 1, a: 0}));

    coll.aggregate(pipeline);
    assert.eq(1, targetColl.find().itcount());
    assert.eq(2, targetColl.getIndexes().length);

    //
    // Test that an $out aggregation succeeds even if the _id is stripped out and the "uniqueKey"
    // is the document key.
    //
    coll.drop();
    targetColl.drop();
    assert.commandWorked(coll.insert({val: "will be removed"}));
    assert.doesNotThrow(() => coll.aggregate([
        {$replaceRoot: {newRoot: {name: "kyle"}}},
        {$out: {to: targetColl.getName(), mode: "replaceCollection"}}
    ]));
    assert.eq(1, targetColl.find({name: "kyle", val: {$exists: false}}).itcount());

    //
    // Test that an $out aggregation succeeds even if the _id is stripped out and _id is part of a
    // multi-field "uniqueKey".
    //
    targetColl.drop();
    assert.commandWorked(targetColl.createIndex({name: -1, _id: -1}, {unique: true}));
    assert.doesNotThrow(() => coll.aggregate([
        {$replaceRoot: {newRoot: {name: "jungsoo"}}},
        {
          $out: {
              to: targetColl.getName(),
              mode: "replaceCollection",
              uniqueKey: {_id: 1, name: 1}
          }
        }
    ]));
    assert.eq(1, targetColl.find({val: {$exists: false}}).itcount());

    //
    // Tests for $out to a database that differs from the aggregation database.
    //
    const foreignDb = db.getSiblingDB("mode_replace_collection_foreign");
    const foreignTargetColl = foreignDb.mode_replace_collection_out;
    const pipelineDifferentOutputDb = [{
        $out: {
            to: foreignTargetColl.getName(),
            db: foreignDb.getName(),
            mode: "replaceCollection",
        }
    }];

    // TODO (SERVER-36832): Allow "replaceCollection" mode with a foreign output database.
    assert.commandWorked(foreignTargetColl.insert({val: "forcing database creation"}));
    assertErrorCode(coll, pipelineDifferentOutputDb, 50939);
}());
