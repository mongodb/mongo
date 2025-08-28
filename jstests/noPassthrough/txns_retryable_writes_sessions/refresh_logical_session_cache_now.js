// This test makes assertions about the number of sessions, which are not compatible with
// implicit sessions.
TestData.disableImplicitSessions = true;

let res;
let refresh = {refreshLogicalSessionCacheNow: 1};
let startSession = {startSession: 1};

// Start up a standalone server.
let conn = MongoRunner.runMongod();
let admin = conn.getDB("admin");
let config = conn.getDB("config");

// Trigger an initial refresh, as a sanity check.
res = admin.runCommand(refresh);
assert.commandWorked(res, "failed to refresh");

// Start a session. Should not be in the collection yet.
res = admin.runCommand(startSession);
assert.commandWorked(res, "unable to start session");

assert.eq(config.system.sessions.count(), 0, "should not have session records yet");

// Trigger a refresh. Session should now be in the collection.
res = admin.runCommand(refresh);
assert.commandWorked(res, "failed to refresh");

assert.eq(config.system.sessions.count(), 1, "should have written session records");

// Start some new sessions. Should not be in the collection yet.
let numSessions = 100;
for (let i = 0; i < numSessions; i++) {
    res = admin.runCommand(startSession);
    assert.commandWorked(res, "unable to start session");
}

assert.eq(config.system.sessions.count(), 1, "should not have more session records yet");

// Trigger another refresh. All sessions should now be in the collection.
res = admin.runCommand(refresh);
assert.commandWorked(res, "failed to refresh");

assert.eq(config.system.sessions.count(), numSessions + 1, "should have written session records");
MongoRunner.stopMongod(conn);
