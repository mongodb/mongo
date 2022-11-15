/**
 * Tests that the deprecated 'hash' oplog entry field is silently stripped in applyOps.
 */

(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = "testDB";
const collName = "testColl";

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);

jsTestLog("Creating collection explicitly");
assert.commandWorked(primaryDB.runCommand({create: collName}));

jsTestLog("Running applyOps command");
assert.commandWorked(primaryDB.runCommand(
    {applyOps: [{op: "i", ns: dbName + "." + collName, o: {_id: "mustStripHash"}, h: 0}]}));

jsTestLog("Verifying result of applyOps");
const entry = primary.getDB("local").oplog.rs.find({"o._id": "mustStripHash"}).next();
assert(!entry.hasOwnProperty("h"), () => tojson(entry));

rst.stopSet();
})();
