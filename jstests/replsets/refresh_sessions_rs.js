// This test makes assertions about the number of logical session records.
import {ReplSetTest} from "jstests/libs/replsettest.js";

TestData.disableImplicitSessions = true;

let refresh = {refreshLogicalSessionCacheNow: 1};
let startSession = {startSession: 1};

// Start up a replica set.
let dbName = "config";

let replTest = new ReplSetTest({name: "refresh", nodes: 3});
let nodes = replTest.startSet();

replTest.initiate();
let primary = replTest.getPrimary();

replTest.awaitSecondaryNodes();
let [server2, server3] = replTest.getSecondaries();

let db1 = primary.getDB(dbName);
let db2 = server2.getDB(dbName);
let db3 = server3.getDB(dbName);

let res;

// The primary needs to create the sessions collection so that the secondaries can act upon it.
// This is done by an initial refresh of the primary.
res = db1.runCommand(refresh);
assert.commandWorked(res, "failed to refresh");
replTest.awaitReplication();

// Trigger an initial refresh on secondaries as a sanity check.
res = db2.runCommand(refresh);
assert.commandWorked(res, "failed to refresh");
res = db3.runCommand(refresh);
assert.commandWorked(res, "failed to refresh");

// Connect to the primary and start a session.
db1.runCommand(startSession);
assert.commandWorked(res, "unable to start session");

// That session should not be in db.system.sessions yet.
assert.eq(db1.system.sessions.count(), 0, "should not have session records yet");

// Connect to each replica set member and start a session.
res = db2.runCommand(startSession);
assert.commandWorked(res, "unable to start session");
res = db3.runCommand(startSession);
assert.commandWorked(res, "unable to start session");

// Connect to a secondary and trigger a refresh.
res = db2.runCommand(refresh);
assert.commandWorked(res, "failed to refresh");

// Connect to the primary. The sessions collection here should have one record for the session
// on the secondary.
assert.eq(db1.system.sessions.count(), 1, "failed to refresh on the secondary");

// Trigger a refresh on the primary. The sessions collection should now contain two records.
res = db1.runCommand(refresh);
assert.commandWorked(res, "failed to refresh");
assert.eq(db1.system.sessions.count(), 2, "should have two local session records after refresh");

// Trigger another refresh on all members.
res = db2.runCommand(refresh);
assert.commandWorked(res, "failed to refresh");
res = db3.runCommand(refresh);
assert.commandWorked(res, "failed to refresh");
res = db1.runCommand(refresh);
assert.commandWorked(res, "failed to refresh");

// The sessions collection on the primary should now contain all records.
assert.eq(db1.system.sessions.count(), 3, "should have three local session records after refresh");

// Stop the test.
replTest.stopSet();
