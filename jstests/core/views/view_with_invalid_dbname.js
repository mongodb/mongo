
/**
 * When loading the view catalog, the server should not crash because it encountered a view with an
 * invalid name. This test is specifically for the case of a view with a dbname that contains an
 * embedded null character (SERVER-36859).
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   # applyOps is not available on mongos.
 *   assumes_against_mongod_not_mongos,
 *   # applyOps is not retryable.
 *   requires_non_retryable_commands,
 * ]
 */
(function() {
"use strict";

const testDB = db.getSiblingDB("view_with_invalid_dbname");

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

testDB.system.views.drop();
assert.commandWorked(testDB.createCollection("system.views"));
assert.commandWorked(testDB.adminCommand(
    {applyOps: [{op: "i", ns: testDB.getName() + ".system.views", o: viewDef}]}));

// Don't let the bogus view stick around, or else it will cause an error in validation.
assert.commandWorked(testDB.adminCommand(
    {applyOps: [{op: "d", ns: testDB.getName() + ".system.views", o: {_id: viewName}}]}));
}());
