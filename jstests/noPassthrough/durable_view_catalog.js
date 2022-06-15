/**
 * Tests that view creation and modification is correctly persisted.
 *
 * This test requires persistence to ensure data survives a restart.
 * @tags: [requires_persistence]
 */
(function() {
'use strict';

// The following test verifies that writeConcern: {j: true} ensures that the view catalog is
// durable.
let dbpath = MongoRunner.dataPath + '_durable_view_catalog';
resetDbpath(dbpath);

let mongodArgs = {dbpath: dbpath, noCleanData: true};

// Start a mongod.
let conn = MongoRunner.runMongod(mongodArgs);
assert.neq(null, conn, 'mongod was unable to start up');

// Now connect to the mongod, create, remove and modify views and then abruptly stop the server.
let viewsDB = conn.getDB('test');
let pipe = [{$match: {}}];
assert.commandWorked(viewsDB.runCommand({create: "view1", viewOn: "collection", pipeline: pipe}));
assert.commandWorked(viewsDB.runCommand({create: "view2", viewOn: "collection", pipeline: pipe}));
assert.commandWorked(viewsDB.runCommand({create: "view3", viewOn: "collection", pipeline: pipe}));
assert.commandWorked(viewsDB.runCommand({collMod: "view3", viewOn: "view2"}));
// On the final modification, require a sync to ensure durability.
assert.commandWorked(viewsDB.runCommand({drop: "view1", writeConcern: {j: 1}}));

// Hard kill the mongod to ensure the data was indeed synced to durable storage.
MongoRunner.stopMongod(conn, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});

// Restart the mongod.
conn = MongoRunner.runMongod(mongodArgs);
assert.neq(null, conn, 'mongod was unable to restart after receiving a SIGKILL');

// Check that our journaled write still is present.
viewsDB = conn.getDB('test');
let actualViews = viewsDB.system.views.find().toArray();
let expectedViews = [
    {"_id": "test.view2", "viewOn": "collection", "pipeline": pipe},
    {"_id": "test.view3", "viewOn": "view2", "pipeline": pipe}
];
assert.eq(actualViews, expectedViews, "view definitions not correctly persisted");
let listedViews =
    viewsDB.runCommand({listCollections: 1, filter: {type: "view"}})
        .cursor.firstBatch.map((function(x) {
            return {_id: "test." + x.name, viewOn: x.options.viewOn, pipeline: x.options.pipeline};
        }));
assert.sameMembers(listedViews, expectedViews, "persisted view definitions not correctly loaded");

// Insert an invalid view definition directly into system.views to bypass normal validation.
assert.commandWorked(viewsDB.adminCommand({
    applyOps: [
        {op: "i", ns: viewsDB.getName() + ".system.views", o: {_id: "badView", pipeline: "badType"}}
    ]
}));

// Skip collection validation during stopMongod if invalid views exists.
TestData.skipValidationOnInvalidViewDefinitions = true;

// Restarting the mongod should succeed despite the presence of invalid view definitions.
MongoRunner.stopMongod(conn);
conn = MongoRunner.runMongod(mongodArgs);
assert.neq(
    null,
    conn,
    "after inserting bad views, failed to restart mongod with options: " + tojson(mongodArgs));

// Now that the database's view catalog has been marked as invalid, all view operations in that
// database should fail.
viewsDB = conn.getDB("test");
assert.commandFailedWithCode(viewsDB.runCommand({find: "view2"}), ErrorCodes.InvalidViewDefinition);
assert.commandFailedWithCode(viewsDB.runCommand({create: "view4", viewOn: "collection"}),
                             ErrorCodes.InvalidViewDefinition);
assert.commandFailedWithCode(viewsDB.runCommand({collMod: "view2", viewOn: "view4"}),
                             ErrorCodes.InvalidViewDefinition);

// Checks that dropping a nonexistent view or collection is not affected by an invalid view existing
// in the view catalog.
assert.commandFailedWithCode(viewsDB.runCommand({drop: "view4"}), ErrorCodes.NamespaceNotFound);
assert.commandFailedWithCode(viewsDB.runCommand({listCollections: 1}),
                             ErrorCodes.InvalidViewDefinition);

// Manually remove the invalid view definition from system.views, and then verify that view
// operations work successfully without requiring a server restart.
assert.commandWorked(viewsDB.adminCommand(
    {applyOps: [{op: "d", ns: viewsDB.getName() + ".system.views", o: {_id: "badView"}}]}));
assert.commandWorked(viewsDB.runCommand({find: "view2"}));
assert.commandWorked(viewsDB.runCommand({create: "view4", viewOn: "collection"}));
assert.commandWorked(viewsDB.runCommand({collMod: "view2", viewOn: "view4"}));
assert.commandWorked(viewsDB.runCommand({drop: "view4"}));
assert.commandWorked(viewsDB.runCommand({listCollections: 1}));
MongoRunner.stopMongod(conn);
})();
