// Test the count command with views.
(function() {
    "use strict";

    var viewsDB = db.getSiblingDB("views_count");
    assert.commandWorked(viewsDB.dropDatabase());

    // Insert documents into a collection.
    let coll = viewsDB.getCollection("coll");
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < 10; i++) {
        bulk.insert({x: i});
    }
    assert.writeOK(bulk.execute());

    // Create views on the data.
    assert.commandWorked(viewsDB.runCommand({create: "identityView", viewOn: "coll"}));
    assert.commandWorked(viewsDB.runCommand(
        {create: "greaterThanThreeView", viewOn: "coll", pipeline: [{$match: {x: {$gt: 3}}}]}));
    assert.commandWorked(viewsDB.runCommand({
        create: "lessThanSevenView",
        viewOn: "greaterThanThreeView",
        pipeline: [{$match: {x: {$lt: 7}}}]
    }));
    let identityView = viewsDB.getCollection("identityView");
    let greaterThanThreeView = viewsDB.getCollection("greaterThanThreeView");
    let lessThanSevenView = viewsDB.getCollection("lessThanSevenView");

    // Count on a view, with or without a query.
    assert.eq(coll.count(), identityView.count());
    assert.eq(coll.count({}), identityView.count({}));
    assert.eq(coll.count({x: {$exists: true}}), identityView.count({x: {$exists: true}}));
    assert.eq(coll.count({x: 0}), identityView.count({x: 0}));
    assert.eq(6, greaterThanThreeView.count());
    assert.eq(6, greaterThanThreeView.count({}));
    assert.eq(3, lessThanSevenView.count());
    assert.eq(3, lessThanSevenView.count({}));

    // Test empty counts.
    assert.eq(coll.count({x: -1}), identityView.count({x: -1}));
    assert.eq(0, greaterThanThreeView.count({x: 2}));
    assert.eq(0, lessThanSevenView.count({x: 9}));

    // Counting on views works with limit and skip.
    assert.eq(7, identityView.count({x: {$exists: true}}, {skip: 3}));
    assert.eq(3, greaterThanThreeView.count({x: {$lt: 100}}, {limit: 3}));
    assert.eq(1, lessThanSevenView.count({}, {skip: 1, limit: 1}));

    // Count with explain works on a view.
    assert.commandWorked(lessThanSevenView.explain().count());
    assert.commandWorked(greaterThanThreeView.explain().count({x: 6}));
    let explainPlan = lessThanSevenView.explain().count({foo: "bar"});
    assert.commandWorked(explainPlan);
    assert.eq(explainPlan["stages"][0]["$cursor"]["queryPlanner"]["namespace"], "views_count.coll");

    assert.commandFailedWithCode(
        viewsDB.runCommand({count: "identityView", collation: {locale: "en_US"}}),
        ErrorCodes.OptionNotSupportedOnView);

    // Hint cannot be used when counting on a view.
    assert.commandFailedWithCode(viewsDB.runCommand({count: "identityView", hint: "_id_"}),
                                 ErrorCodes.InvalidPipelineOperator);
}());
