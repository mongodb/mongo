// Test the creation of views with various options.

(function() {
    "use strict";

    // For arrayEq.
    load("jstests/aggregation/extras/utils.js");

    var viewsDB = db.getSiblingDB("views_creation");
    assert.commandWorked(viewsDB.dropDatabase());

    var collNames = viewsDB.getCollectionNames();
    assert.eq(0, collNames.length, tojson(collNames));

    // Create a collection for test purposes.
    assert.commandWorked(viewsDB.runCommand({create: "collection"}));

    var pipe = [{$match: {}}];

    // Create a "regular" view on a collection.
    assert.commandWorked(
        viewsDB.runCommand({create: "view", viewOn: "collection", pipeline: pipe}));

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
