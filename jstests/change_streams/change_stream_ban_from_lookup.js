/**
 * Test that the $changeStream stage cannot be used in a $lookup pipeline or sub-pipeline.
 */
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

    const coll = db.change_stream_ban_from_lookup;
    coll.drop();

    assert.writeOK(coll.insert({_id: 1}));

    // Verify that we cannot create a $lookup using a pipeline which begins with $changeStream.
    assertErrorCode(coll,
                    [{$lookup: {from: coll.getName(), as: 'as', pipeline: [{$changeStream: {}}]}}],
                    ErrorCodes.IllegalOperation);

    // Verify that we cannot create a $lookup if its pipeline contains a sub-$lookup whose pipeline
    // begins with $changeStream.
    assertErrorCode(
        coll,
        [{
           $lookup: {
               from: coll.getName(),
               as: 'as',
               pipeline: [
                   {$match: {_id: 1}},
                   {
                     $lookup:
                         {from: coll.getName(), as: 'subas', pipeline: [{$changeStream: {}}]}
                   }
               ]
           }
        }],
        ErrorCodes.IllegalOperation);
})();
