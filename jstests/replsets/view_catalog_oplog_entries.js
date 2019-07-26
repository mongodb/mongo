/**
 * Test that creating and modifying a view publishes entries to the oplog for each operation and
 * that both entries include a UUID for the "system.views" collection.
 */

(function() {
"use strict";

const dbName = "view_catalog_oplog_entries";
const collName = "test_coll";
const viewName = "test_view";

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();

assert.commandWorked(primary.getDB(dbName)[collName].insert({a: 1}));

// Create the view.
assert.commandWorked(primary.getDB(dbName).createView(viewName, collName, []));

// Modify the view with the "collMod" command.
assert.commandWorked(primary.getDB(dbName).runCommand(
    {collMod: viewName, viewOn: collName, pipeline: [{$project: {a: 1}}]}));

// There should be exactly one insert into "system.views" for the view creation...
const oplog = primary.getDB("local").oplog.rs;
const createViewOplogEntry = oplog.find({op: "i", ns: (dbName + ".system.views")}).toArray();
assert.eq(createViewOplogEntry.length, 1);
assert(createViewOplogEntry[0].hasOwnProperty("ui"),
       "Oplog entry for view creation missing UUID for view catalog: " +
           tojson(createViewOplogEntry[0]));
const viewCatalogUUID = createViewOplogEntry[0].ui;

// ...and exactly one update on "system.views" for the view collMod.
const modViewOplogEntry = oplog.find({op: "u", ns: (dbName + ".system.views")}).toArray();
assert.eq(modViewOplogEntry.length, 1);
assert(modViewOplogEntry[0].hasOwnProperty("ui"),
       "Oplog entry for view modification missing UUID for view catalog: " +
           tojson(modViewOplogEntry[0]));

// Both entries should have the same UUID.
assert.eq(viewCatalogUUID, modViewOplogEntry[0].ui);

replTest.stopSet();
}());
