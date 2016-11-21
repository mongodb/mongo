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

    function assertIdentityViewDistinctMatchesCollection(key, query) {
        query = (query === undefined) ? {} : query;
        const collDistinct = coll.distinct(key, query);
        const viewDistinct = identityView.distinct(key, query);
        assert(arrayEq(collDistinct, viewDistinct),
               "Distinct on a collection did not match distinct on its identity view; got " +
                   tojson(viewDistinct) + " but expected " + tojson(collDistinct));
    }

    // Test basic distinct requests on known fields without a query.
    assertIdentityViewDistinctMatchesCollection("pop");
    assertIdentityViewDistinctMatchesCollection("_id");
    assert(arrayEq([7, 10], largePopView.distinct("pop")));
    assert(arrayEq(["New York", "Palo Alto"], largePopView.distinct("_id")));

    // Test distinct with the presence of a query.
    assertIdentityViewDistinctMatchesCollection("state", {});
    assertIdentityViewDistinctMatchesCollection("pop", {pop: {$exists: true}});
    assertIdentityViewDistinctMatchesCollection("state", {pop: {$gt: 3}});
    assertIdentityViewDistinctMatchesCollection("_id", {state: "CA"});
    assert(arrayEq(["CA"], largePopView.distinct("state", {pop: {$gte: 8}})));
    assert(arrayEq([7], largePopView.distinct("pop", {state: "NY"})));

    // Test distinct where we expect an empty set response.
    assertIdentityViewDistinctMatchesCollection("nonexistent");
    assertIdentityViewDistinctMatchesCollection("pop", {pop: {$gt: 1000}});
    assert.eq([], largePopView.distinct("nonexistent"));
    assert.eq([], largePopView.distinct("_id", {state: "FL"}));

    // Explain works with distinct.
    assert.commandWorked(identityView.explain().distinct("_id"));
    assert.commandWorked(largePopView.explain().distinct("pop", {state: "CA"}));
    let explainPlan = largePopView.explain().count({foo: "bar"});
    assert.commandWorked(explainPlan);
    assert.eq(explainPlan["stages"][0]["$cursor"]["queryPlanner"]["namespace"],
              "views_distinct.coll");

    // Distinct commands fail when they try to change the collation of a view.
    assert.commandFailedWithCode(
        viewsDB.runCommand({distinct: "identityView", key: "state", collation: {locale: "en_US"}}),
        ErrorCodes.OptionNotSupportedOnView);

    // Test distinct on nested objects, nested arrays and nullish values.
    coll.drop();
    allDocuments = [];
    allDocuments.push({a: 1, b: [2, 3, [4, 5], {c: 6}], d: {e: [1, 2]}});
    allDocuments.push({a: [1], b: [2, 3, 4, [5]], c: 6, d: {e: 1}});
    allDocuments.push({a: [1, 2], b: 3, c: [6], d: [{e: 1}, {e: [1, 2]}]});
    allDocuments.push({a: [1, 2], b: [4, 5], c: [undefined], d: [1]});
    allDocuments.push({a: null, b: [4, 5, null, undefined], c: [], d: {e: null}});
    allDocuments.push({a: undefined, b: null, c: [null], d: {e: undefined}});

    bulk = coll.initializeUnorderedBulkOp();
    allDocuments.forEach(function(doc) {
        bulk.insert(doc);
    });
    assert.writeOK(bulk.execute());

    assertIdentityViewDistinctMatchesCollection("a");
    assertIdentityViewDistinctMatchesCollection("b");
    assertIdentityViewDistinctMatchesCollection("c");
    assertIdentityViewDistinctMatchesCollection("d");
    assertIdentityViewDistinctMatchesCollection("e");
}());
