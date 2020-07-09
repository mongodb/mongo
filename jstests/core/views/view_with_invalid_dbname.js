
/**
 * When loading the view catalog, the server should not crash because it encountered a view with an
 * invalid name. This test is specifically for the case of a view with a dbname that contains an
 * embedded null character (SERVER-36859).
 *
 * @tags: [
 *   # applyOps is not available on mongos.
 *   assumes_against_mongod_not_mongos,
 *   # applyOps is not retryable.
 *   requires_non_retryable_commands,
 * ]
 */
(function() {
"use strict";

// Create a view whose dbname has an invalid embedded NULL character. That's not possible with
// the 'create' command, but it is possible by manually inserting into the 'system.views'
// collection.
const viewName = "dbNameWithEmbedded\0Character.collectionName";
const collName = "viewOnForViewWithInvalidDBNameTest";
const viewDef = {
    _id: viewName,
    viewOn: collName,
    pipeline: []
};

db.system.views.drop();
assert.commandWorked(db.createCollection("system.views"));
assert.commandWorked(db.adminCommand({applyOps: [{op: "i", ns: "test.system.views", o: viewDef}]}));

// Don't let the bogus view stick around, or else it will cause an error in validation.
assert.commandWorked(
    db.adminCommand({applyOps: [{op: "d", ns: "test.system.views", o: {_id: viewName}}]}));
}());
