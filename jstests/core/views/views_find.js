/**
 * Tests the find command on views.
 * @tags: [requires_find_command]
 */
(function() {
    "use strict";

    // For arrayEq and orderedArrayEq.
    load("jstests/aggregation/extras/utils.js");

    let viewsDB = db.getSiblingDB("views_find");
    assert.commandWorked(viewsDB.dropDatabase());

    // Helper functions.
    let assertFindResultEq = function(cmd, expected, ordered) {
        let res = viewsDB.runCommand(cmd);
        assert.commandWorked(res);
        let arr = new DBCommandCursor(db.getMongo(), res, 5).toArray();
        let errmsg = tojson({expected: expected, got: arr});

        if (typeof(ordered) === "undefined" || !ordered)
            assert(arrayEq(arr, expected), errmsg);
        else
            assert(orderedArrayEq(arr, expected), errmsg);
    };

    // Populate a collection with some test data.
    let allDocuments = [];
    allDocuments.push({_id: "New York", state: "NY", pop: 7});
    allDocuments.push({_id: "Newark", state: "NJ", pop: 3});
    allDocuments.push({_id: "Palo Alto", state: "CA", pop: 10});
    allDocuments.push({_id: "San Francisco", state: "CA", pop: 4});
    allDocuments.push({_id: "Trenton", state: "NJ", pop: 5});

    let coll = viewsDB.coll;
    let bulk = coll.initializeUnorderedBulkOp();
    allDocuments.forEach(function(doc) {
        bulk.insert(doc);
    });
    assert.writeOK(bulk.execute());

    // Create views on the data.
    assert.commandWorked(
        viewsDB.runCommand({create: "identityView", viewOn: "coll", pipeline: [{$match: {}}]}));
    assert.commandWorked(viewsDB.runCommand({
        create: "noIdView",
        viewOn: "coll",
        pipeline: [{$match: {}}, {$project: {_id: 0, state: 1, pop: 1}}]
    }));

    // Filters and "simple" projections.
    assertFindResultEq({find: "identityView"}, allDocuments);
    assertFindResultEq({find: "identityView", filter: {state: "NJ"}, projection: {_id: 1}},
                       [{_id: "Trenton"}, {_id: "Newark"}]);

    // A view that projects out the _id should still work with the find command.
    assertFindResultEq({find: "noIdView", filter: {state: "NY"}, projection: {pop: 1}}, [{pop: 7}]);

    // Sort, limit and batchSize.
    const doOrderedSort = true;
    assertFindResultEq({find: "identityView", sort: {_id: 1}}, allDocuments, doOrderedSort);
    assertFindResultEq(
        {find: "identityView", limit: 1, batchSize: 1, sort: {_id: 1}, projection: {_id: 1}},
        [{_id: "New York"}]);
    assert.commandFailedWithCode(viewsDB.runCommand({find: "identityView", sort: {$natural: 1}}),
                                 ErrorCodes.InvalidPipelineOperator);

    // Negative batch size and limit should fail.
    assert.commandFailed(viewsDB.runCommand({find: "identityView", batchSize: -1}));
    assert.commandFailed(viewsDB.runCommand({find: "identityView", limit: -1}));

    // Views support find with explain.
    assert.commandWorked(viewsDB.identityView.find().explain());

    // Only simple 0 or 1 projections are allowed on views.
    assert.writeOK(viewsDB.coll.insert({arr: [{x: 1}]}));
    assert.commandFailedWithCode(
        viewsDB.runCommand({find: "identityView", projection: {arr: {$elemMatch: {x: 1}}}}),
        ErrorCodes.InvalidPipelineOperator);

    // Views can support a "findOne" if singleBatch: true and limit: 1.
    assertFindResultEq({find: "identityView", filter: {state: "NY"}, singleBatch: true, limit: 1},
                       [{_id: "New York", state: "NY", pop: 7}]);
    assert.eq(viewsDB.identityView.findOne({_id: "San Francisco"}),
              {_id: "San Francisco", state: "CA", pop: 4});
}());
