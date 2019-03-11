// Test the creation of views with various options.
// @tags: [
//   assumes_superuser_permissions,
// ]

(function() {
    "use strict";

    // For arrayEq.
    load("jstests/aggregation/extras/utils.js");

    const viewsDBName = "views_creation";

    let viewsDB = db.getSiblingDB(viewsDBName);
    assert.commandWorked(viewsDB.dropDatabase());

    let collNames = viewsDB.getCollectionNames();
    assert.eq(0, collNames.length, tojson(collNames));

    // You cannot create a view that starts with 'system.'.
    assert.commandFailedWithCode(
        viewsDB.runCommand({create: "system.special", viewOn: "collection"}),
        ErrorCodes.InvalidNamespace,
        "Created an illegal view named 'system.views'");

    // Collections that start with 'system.' that are not special to MongoDB fail with a different
    // error code.
    assert.commandFailedWithCode(viewsDB.runCommand({create: "system.foo", viewOn: "collection"}),
                                 ErrorCodes.InvalidNamespace,
                                 "Created an illegal view named 'system.foo'");

    // Create a collection for test purposes.
    assert.commandWorked(viewsDB.runCommand({create: "collection"}));

    let pipe = [{$match: {}}];

    // Create a "regular" view on a collection.
    assert.commandWorked(
        viewsDB.runCommand({create: "view", viewOn: "collection", pipeline: pipe}));

    collNames = viewsDB.getCollectionNames().filter((function(coll) {
        return !coll.startsWith("system.");
    }));
    assert.eq(2, collNames.length, tojson(collNames));
    let res = viewsDB.runCommand({listCollections: 1, filter: {type: "view"}});
    assert.commandWorked(res);

    // Ensure that the output of listCollections has all the expected options for a view.
    let expectedListCollectionsOutput = [{
        name: "view",
        type: "view",
        options: {viewOn: "collection", pipeline: pipe},
        info: {readOnly: true}
    }];
    assert(arrayEq(res.cursor.firstBatch, expectedListCollectionsOutput), tojson({
               expectedListCollectionsOutput: expectedListCollectionsOutput,
               got: res.cursor.firstBatch
           }));

    // Create a view on a non-existent collection.
    assert.commandWorked(
        viewsDB.runCommand({create: "viewOnNonexistent", viewOn: "nonexistent", pipeline: pipe}));

    // Create a view but don't specify a pipeline; this should default to something sane.
    assert.commandWorked(
        viewsDB.runCommand({create: "viewWithDefaultPipeline", viewOn: "collection"}));

    // Specifying a pipeline but no view namespace must fail.
    assert.commandFailed(viewsDB.runCommand({create: "viewNoViewNamespace", pipeline: pipe}));

    // Create a view on another view.
    assert.commandWorked(
        viewsDB.runCommand({create: "viewOnView", viewOn: "view", pipeline: pipe}));

    // View names are constrained to the same limitations as collection names.
    assert.commandFailed(viewsDB.runCommand({create: "", viewOn: "collection", pipeline: pipe}));
    assert.commandFailedWithCode(
        viewsDB.runCommand({create: "system.local.new", viewOn: "collection", pipeline: pipe}),
        ErrorCodes.InvalidNamespace);
    assert.commandFailedWithCode(
        viewsDB.runCommand({create: "dollar$", viewOn: "collection", pipeline: pipe}),
        ErrorCodes.InvalidNamespace);

    // You cannot create a view with a $out stage, by itself or nested inside of a different stage.
    const ERROR_CODE_OUT_BANNED_IN_LOOKUP = 51047;
    const outStage = {$out: "nonExistentCollection"};
    assert.commandFailedWithCode(
        viewsDB.runCommand({create: "viewWithOut", viewOn: "collection", pipeline: [outStage]}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(viewsDB.runCommand({
        create: "viewWithOutInLookup",
        viewOn: "collection",
        pipeline: [{$lookup: {from: "other", pipeline: [outStage], as: "result"}}]
    }),
                                 ERROR_CODE_OUT_BANNED_IN_LOOKUP);
    assert.commandFailedWithCode(viewsDB.runCommand({
        create: "viewWithOutInFacet",
        viewOn: "collection",
        pipeline: [{$facet: {output: [outStage]}}]
    }),
                                 40600);

    // These test that, when an existing view in system.views is invalid because of a $out in the
    // pipeline, the database errors on creation of a new view.
    assert.commandWorked(viewsDB.system.views.insert({
        _id: `${viewsDBName}.invalidView`,
        viewOn: "collection",
        pipeline: [{$project: {_id: false}}, {$out: "notExistingCollection"}]
    }));
    assert.commandFailedWithCode(
        viewsDB.runCommand({create: "viewWithBadViewCatalog", viewOn: "collection", pipeline: []}),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandWorked(
        viewsDB.system.views.remove({_id: `${viewsDBName}.invalidView`}, {justOne: true}));
}());
