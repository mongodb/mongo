// This tests that we can successfully profile queries on secondaries.
import {ReplSetTest} from "jstests/libs/replsettest.js";

let rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
rst.awaitReplication();

let dbName = jsTestName();
let primary = rst.getPrimary();
let secondaryDB = rst.getSecondary().getDB(dbName);

// Ensure the database exists on the secondary via replication before enabling profiling.
// With SERVER-119744, setProfilingLevel on a secondary no longer creates the database.
assert.commandWorked(primary.getDB(dbName).createCollection("coll"));
assert(primary.getDB(dbName).coll.drop());
rst.awaitReplication();

jsTestLog("Enable profiling on the secondary");
assert.commandWorked(secondaryDB.runCommand({profile: 2}));

jsTestLog("Perform a query that returns no results, but will get profiled.");
secondaryDB.doesntexist.find({}).itcount();

let numProfileEntries = (coll) =>
    coll.getDB().system.profile.find({op: "query", ns: coll.getFullName(), nreturned: 0}).itcount();

jsTestLog("Check the query is in the profile and turn profiling off.");
assert.eq(numProfileEntries(secondaryDB.doesntexist), 1, "expected a single profile entry");
assert.commandWorked(secondaryDB.runCommand({profile: 0}));
rst.stopSet();
