// Tests that the $changeNotification stage can only be present as the first stage in the pipeline.
(function() {
    "use strict";
    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

    let testName = "change_notification_required_as_first_stage";
    var rst = new ReplSetTest({name: testName, nodes: 1});
    rst.startSet();
    rst.initiate();

    const coll = rst.getPrimary().getDB("test").getCollection(testName);

    assertErrorCode(coll, [{$indexStats: {}}, {$changeNotification: {}}], 40549);
    assertErrorCode(
        coll,
        [{$indexStats: {}}, {$changeNotification: {}}, {$match: {test: "this is an extra stage"}}],
        40549);

    // Test that a $changeNotification stage is not allowed within a $facet stage.
    assertErrorCode(coll, [{$facet: {testPipe: [{$changeNotification: {}}]}}], 40550);
    assertErrorCode(coll,
                    [{
                       $facet: {
                           testPipe: [
                               {$indexStats: {}},
                               {$changeNotification: {}},
                               {$match: {test: "this is an extra stage"}}
                           ]
                       }
                    }],
                    40550);
}());
