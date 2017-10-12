/**
 * Test that the $changeStream stage cannot be used in a view definition pipeline.
 */
(function() {
    "use strict";

    const collName = "change_stream_ban_from_views";
    const normalViewName = "nonChangeStreamView";
    const csViewName = "changeStreamView";

    db[normalViewName].drop();
    db[csViewName].drop();
    db[collName].drop();

    assert.writeOK(db[collName].insert({_id: 1}));

    const csPipe = [{$changeStream: {}}];

    // Create one valid view for testing purposes.
    assert.commandWorked(
        db.runCommand({create: normalViewName, viewOn: collName, pipeline: [{$match: {_id: 1}}]}));

    // Verify that we cannot create a view using a pipeline which begins with $changeStream.
    assert.commandFailedWithCode(
        db.runCommand({create: csViewName, viewOn: collName, pipeline: csPipe}),
        ErrorCodes.OptionNotSupportedOnView);

    // We also cannot update an existing view to use a $changeStream pipeline.
    assert.commandFailedWithCode(
        db.runCommand({collMod: normalViewName, viewOn: collName, pipeline: csPipe}),
        ErrorCodes.OptionNotSupportedOnView);
})();
