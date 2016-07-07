// Tests aggregation on views for proper pipeline concatenation and semantics.
(function() {
    "use strict";

    // For arrayEq and orderedArrayEq.
    load("jstests/aggregation/extras/utils.js");

    var viewsDB = db.getSiblingDB("views_aggregation");
    assert.commandWorked(viewsDB.dropDatabase());

    // Helper functions.
    let assertAggResultEq = function(collection, pipeline, expected, ordered) {
        let coll = viewsDB.getCollection(collection);
        let arr = coll.aggregate(pipeline).toArray();
        let success = (typeof(ordered) === "undefined" || !ordered) ? arrayEq(arr, expected)
                                                                    : orderedArrayEq(arr, expected);
        assert(success, tojson({got: arr, expected: expected}));
    };
    let byPopulation = function(a, b) {
        if (a.pop < b.pop)
            return -1;
        else if (a.pop > b.pop)
            return 1;
        else
            return 0;
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
    assert.commandWorked(viewsDB.runCommand({create: "emptyPipelineView", viewOn: "coll"}));
    assert.commandWorked(
        viewsDB.runCommand({create: "identityView", viewOn: "coll", pipeline: [{$match: {}}]}));
    assert.commandWorked(viewsDB.runCommand(
        {create: "noIdView", viewOn: "coll", pipeline: [{$project: {_id: 0, state: 1, pop: 1}}]}));
    assert.commandWorked(viewsDB.runCommand({
        create: "popSortedView",
        viewOn: "identityView",
        pipeline: [{$match: {pop: {$gte: 0}}}, {$sort: {pop: 1}}]
    }));

    // Find all documents with empty aggregations.
    assertAggResultEq("emptyPipelineView", [], allDocuments);
    assertAggResultEq("identityView", [], allDocuments);
    assertAggResultEq("identityView", [{$match: {}}], allDocuments);

    // Filter documents on a view with $match.
    assertAggResultEq(
        "popSortedView", [{$match: {state: "NY"}}], [{_id: "New York", state: "NY", pop: 7}]);

    // An aggregation still works on a view that strips _id.
    assertAggResultEq("noIdView", [{$match: {state: "NY"}}], [{state: "NY", pop: 7}]);

    // Aggregations work on views that sort.
    const doOrderedSort = true;
    assertAggResultEq("popSortedView", [], allDocuments.sort(byPopulation), doOrderedSort);
    assertAggResultEq("popSortedView", [{$limit: 1}, {$project: {_id: 1}}], [{_id: "Palo Alto"}]);

    // Create a cyclical view and assert that aggregation fails appropriately.
    // TODO(SERVER-24768) This should be prohibited on the create command
    assert.commandWorked(viewsDB.runCommand({create: "viewCycle1", viewOn: "viewCycle2"}));
    assert.commandWorked(viewsDB.runCommand({create: "viewCycle2", viewOn: "viewCycle1"}));
    assert.commandFailedWithCode(viewsDB.runCommand({aggregate: "viewCycle1", pipeline: []}),
                                 ErrorCodes.ViewDepthLimitExceeded);

    // Create views-on-views that exceed the maximum depth of 20.
    // TODO(SERVER-24768) Consider making this fail as well on creation
    const kMaxViewDepth = 20;
    assert.commandWorked(viewsDB.runCommand({create: "viewChain0", viewOn: "coll"}));
    for (let i = 1; i <= kMaxViewDepth; i++) {
        const viewName = "viewChain" + i;
        const viewOnName = "viewChain" + (i - 1);
        assert.commandWorked(viewsDB.runCommand({create: viewName, viewOn: viewOnName}));
    }
    assert.commandFailedWithCode(viewsDB.runCommand({aggregate: "viewChain20", pipeline: []}),
                                 ErrorCodes.ViewDepthLimitExceeded);

    // However, an aggregation in the middle of the chain should succeed.
    assertAggResultEq("viewChain16", [], allDocuments, !doOrderedSort);
}());
