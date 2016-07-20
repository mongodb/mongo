// Test the creation of views with various options.

(function() {
    "use strict";

    // For arrayEq.
    load("jstests/aggregation/extras/utils.js");

    let viewsDB = db.getSiblingDB("views_creation");
    assert.commandWorked(viewsDB.dropDatabase());

    let collNames = viewsDB.getCollectionNames();
    assert.eq(0, collNames.length, tojson(collNames));

    // Create a collection for test purposes.
    assert.commandWorked(viewsDB.runCommand({create: "collection"}));

    let pipe = [{$match: {}}];

    // Create a "regular" view on a collection.
    assert.commandWorked(
        viewsDB.runCommand({create: "view", viewOn: "collection", pipeline: pipe}));

    collNames = viewsDB.getCollectionNames().filter((function(coll) {
        return !coll.startsWith("system.");
    }));
    assert.eq(2, collNames.length, collNames);
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
        ErrorCodes.BadValue);
    assert.commandFailedWithCode(
        viewsDB.runCommand({create: "dollar$", viewOn: "collection", pipeline: pipe}),
        ErrorCodes.BadValue);
}());
