/**
 * Test that the $changeStream stage cannot be used in a view definition pipeline.
 */
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    const coll = assertDropAndRecreateCollection(db, "change_stream_ban_from_views");
    assert.writeOK(coll.insert({_id: 1}));

    const normalViewName = "nonChangeStreamView";
    const csViewName = "changeStreamView";

    assertDropCollection(db, normalViewName);
    assertDropCollection(db, csViewName);

    const csPipe = [{$changeStream: {}}];

    // Create one valid view for testing purposes.
    assert.commandWorked(db.runCommand(
        {create: normalViewName, viewOn: coll.getName(), pipeline: [{$match: {_id: 1}}]}));

    // Verify that we cannot create a view using a pipeline which begins with $changeStream.
    assert.commandFailedWithCode(
        db.runCommand({create: csViewName, viewOn: coll.getName(), pipeline: csPipe}),
        ErrorCodes.OptionNotSupportedOnView);

    // We also cannot update an existing view to use a $changeStream pipeline.
    assert.commandFailedWithCode(
        db.runCommand({collMod: normalViewName, viewOn: coll.getName(), pipeline: csPipe}),
        ErrorCodes.OptionNotSupportedOnView);

    // Verify change streams cannot be created on views.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: normalViewName, pipeline: [{$changeStream: {}}], cursor: {}}),
        ErrorCodes.CommandNotSupportedOnView);
})();
