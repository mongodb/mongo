/**
 * Test that the $changeStream stage cannot be used in a $lookup pipeline or sub-pipeline.
 */
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");       // For assertErrorCode.
    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    const coll = assertDropAndRecreateCollection(db, "change_stream_ban_from_lookup");
    const foreignColl = "unsharded";

    assert.writeOK(coll.insert({_id: 1}));

    // Verify that we cannot create a $lookup using a pipeline which begins with $changeStream.
    assertErrorCode(coll,
                    [{$lookup: {from: foreignColl, as: 'as', pipeline: [{$changeStream: {}}]}}],
                    ErrorCodes.IllegalOperation);

    // Verify that we cannot create a $lookup if its pipeline contains a sub-$lookup whose pipeline
    // begins with $changeStream.
    assertErrorCode(
        coll,
        [{
           $lookup: {
               from: foreignColl,
               as: 'as',
               pipeline: [
                   {$match: {_id: 1}},
                   {$lookup: {from: foreignColl, as: 'subas', pipeline: [{$changeStream: {}}]}}
               ]
           }
        }],
        ErrorCodes.IllegalOperation);
})();
