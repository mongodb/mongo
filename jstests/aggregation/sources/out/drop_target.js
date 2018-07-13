// Tests the behavior of $out with the dropTarget option.
// @tags: [assumes_unsharded_collection]
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

    const coll = db.drop_target;
    coll.drop();

    const targetColl = db.drop_target_out;
    targetColl.drop();

    //
    // Test $out with dropTarget set to true with a non-existent output collection.
    //
    assert.commandWorked(coll.insert({_id: 0}));

    coll.aggregate([{$out: {to: targetColl.getName(), mode: "insert", dropTarget: true}}]);
    assert.eq(1, targetColl.find().itcount());

    //
    // Test $out with dropTarget set to true with an existing output collection.
    //
    coll.aggregate([{$out: {to: targetColl.getName(), mode: "insert", dropTarget: true}}]);
    assert.eq(1, targetColl.find().itcount());

    //
    // Test $out with dropTarget set to false with an existing output collection.
    //
    coll.drop();
    assert.commandWorked(coll.insert({_id: 1}));

    coll.aggregate([{$out: {to: targetColl.getName(), mode: "insert", dropTarget: false}}]);
    assert.eq(2, targetColl.find().itcount());

    //
    // Test $out with dropTarget set to false with a non-existent output collection.
    //
    targetColl.drop();
    coll.drop();
    assert.commandWorked(coll.insert({_id: 0}));

    coll.aggregate([{$out: {to: targetColl.getName(), mode: "insert", dropTarget: false}}]);
    assert.eq(1, targetColl.find().itcount());

    // Test that the aggregation fails if there's a duplicate key error.
    assertErrorCode(
        coll, [{$out: {to: targetColl.getName(), mode: "insert", dropTarget: false}}], 16996);

    //
    // Test that a $out with dropTarget set to true will preserve the indexes and options of the
    // output collection.
    //
    targetColl.drop();
    assert.commandWorked(db.createCollection(targetColl.getName(), {validator: {a: {$gt: 0}}}));
    assert.commandWorked(targetColl.createIndex({a: 1}));

    coll.drop();
    assert.commandWorked(coll.insert({a: 1}));

    coll.aggregate([{$out: {to: targetColl.getName(), mode: "insert", dropTarget: true}}]);
    assert.eq(1, targetColl.find().itcount());
    assert.eq(2, targetColl.getIndexes().length);

    const listColl = db.runCommand({listCollections: 1, filter: {name: targetColl.getName()}});
    assert.commandWorked(listColl);
    assert.eq({a: {$gt: 0}}, listColl.cursor.firstBatch[0].options["validator"]);

}());
