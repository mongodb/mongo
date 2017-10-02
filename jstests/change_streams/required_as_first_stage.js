// Tests that the $changeStream stage can only be present as the first stage in the pipeline.
(function() {
    "use strict";
    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

    const coll = db.change_stream_required_as_first_stage;
    coll.drop();

    assertErrorCode(coll, [{$indexStats: {}}, {$changeStream: {}}], 40602);
    assertErrorCode(
        coll,
        [{$indexStats: {}}, {$changeStream: {}}, {$match: {test: "this is an extra stage"}}],
        40602);

    // Test that a $changeStream stage is not allowed within a $facet stage.
    assertErrorCode(coll, [{$facet: {testPipe: [{$changeStream: {}}]}}], 40600);
    assertErrorCode(coll,
                    [{
                       $facet: {
                           testPipe: [
                               {$indexStats: {}},
                               {$changeStream: {}},
                               {$match: {test: "this is an extra stage"}}
                           ]
                       }
                    }],
                    40600);
}());
