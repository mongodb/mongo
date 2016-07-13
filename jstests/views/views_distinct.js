// Test the distinct command with views.
(function() {
    "use strict";

    // For arrayEq. We don't use array.eq as it does an ordered comparison on arrays but we don't
    // care about order in the distinct response.
    load("jstests/aggregation/extras/utils.js");

    var viewsDB = db.getSiblingDB("views_distinct");
    assert.commandWorked(viewsDB.dropDatabase());

    // Populate a collection with some test data.
    let allDocuments = [];
    allDocuments.push({_id: "New York", state: "NY", pop: 7});
    allDocuments.push({_id: "Newark", state: "NJ", pop: 3});
    allDocuments.push({_id: "Palo Alto", state: "CA", pop: 10});
    allDocuments.push({_id: "San Francisco", state: "CA", pop: 4});
    allDocuments.push({_id: "Trenton", state: "NJ", pop: 5});

    let coll = viewsDB.getCollection("coll");
    let bulk = coll.initializeUnorderedBulkOp();
    allDocuments.forEach(function(doc) {
        bulk.insert(doc);
    });
    assert.writeOK(bulk.execute());

    // Create views on the data.
    assert.commandWorked(viewsDB.runCommand({create: "identityView", viewOn: "coll"}));
    assert.commandWorked(viewsDB.runCommand(
        {create: "largePopView", viewOn: "identityView", pipeline: [{$match: {pop: {$gt: 5}}}]}));
    let identityView = viewsDB.getCollection("identityView");
    let largePopView = viewsDB.getCollection("largePopView");

    // Test basic distinct requests on known fields without a query.
    assert(arrayEq(coll.distinct("pop"), identityView.distinct("pop")));
    assert(arrayEq(coll.distinct("_id"), identityView.distinct("_id")));
    assert(arrayEq([7, 10], largePopView.distinct("pop")));
    assert(arrayEq(["New York", "Palo Alto"], largePopView.distinct("_id")));

    // Test distinct with the presence of a query.
    assert(arrayEq(coll.distinct("state", {}), identityView.distinct("state", {})));
    assert(arrayEq(coll.distinct("pop", {pop: {$exists: true}}),
                   identityView.distinct("pop", {pop: {$exists: true}})));
    assert(
        arrayEq(coll.distinct("_id", {state: "CA"}), identityView.distinct("_id", {state: "CA"})));
    assert(arrayEq(["CA"], largePopView.distinct("state", {pop: {$gte: 8}})));
    assert(arrayEq([7], largePopView.distinct("pop", {state: "NY"})));

    // Test distinct where we expect an empty set response.
    assert.eq(coll.distinct("nonexistent"), identityView.distinct("nonexistent"));
    assert.eq([], largePopView.distinct("nonexistent"));
    assert.eq(coll.distinct("pop", {pop: {$gt: 1000}}),
              identityView.distinct("pop", {pop: {$gt: 1000}}));
    assert.eq([], largePopView.distinct("_id", {state: "FL"}));

    // Explain works with distinct.
    assert.commandWorked(identityView.explain().distinct("_id"));
    assert.commandWorked(largePopView.explain().distinct("pop", {state: "CA"}));
    let explainPlan = largePopView.explain().count({foo: "bar"});
    assert.commandWorked(explainPlan);
    assert.eq(explainPlan["stages"][0]["$cursor"]["queryPlanner"]["namespace"],
              "views_distinct.coll");

    // TODO(SERVER-25186): Cannot specify a collation when running distinct on a view.
    assert.commandFailedWithCode(
        viewsDB.runCommand({distinct: "identityView", key: "state", collation: {locale: "en_US"}}),
        ErrorCodes.InvalidPipelineOperator);
}());
