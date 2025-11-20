/**
 * Test that the $changeStream stage cannot be used in a view definition pipeline.
 *
 * @tags: [
 *   assumes_read_preference_unchanged,
 * ]
 */
import {assertDropAndRecreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

const coll = assertDropAndRecreateCollection(db, "change_stream_ban_from_views");
assert.commandWorked(coll.insert({_id: 1}));

const normalViewName = "nonChangeStreamView";
const csViewName = "changeStreamView";

assertDropCollection(db, normalViewName);
assertDropCollection(db, csViewName);

const csPipe = [{$changeStream: {}}];

// Create one valid view for testing purposes.
assert.commandWorked(db.runCommand({create: normalViewName, viewOn: coll.getName(), pipeline: [{$match: {_id: 1}}]}));

// Verify that we cannot create a view using a pipeline which begins with $changeStream.
assert.commandFailedWithCode(
    db.runCommand({create: csViewName, viewOn: coll.getName(), pipeline: csPipe}),
    ErrorCodes.OptionNotSupportedOnView,
);

// We also cannot update an existing view to use a $changeStream pipeline.
assert.commandFailedWithCode(
    db.runCommand({collMod: normalViewName, viewOn: coll.getName(), pipeline: csPipe}),
    ErrorCodes.OptionNotSupportedOnView,
);

// Verify change streams cannot be created on views.
let response = db.runCommand({aggregate: normalViewName, pipeline: [{$changeStream: {}}], cursor: {}});
if (response.ok) {
    // In case we are running change streams version 2, the cursor may not be opened on the shard.
    // To ensure the failure indeed occurs, we issue a getMore command to ensure that the cursor
    // will be attempted to be opened on the shard and will fail.
    assert.eq(response._changeStreamVersion, "v2", "Change stream of version v1 should fail immediately");
    response = db.runCommand({getMore: response.cursor.id, collection: normalViewName});
}
assert.commandFailedWithCode(response, ErrorCodes.CommandNotSupportedOnView);
