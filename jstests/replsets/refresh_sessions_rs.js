(function() {
    "use strict";

    // This test makes assertions about the number of logical session records.
    TestData.disableImplicitSessions = true;

    var refresh = {refreshLogicalSessionCacheNow: 1};
    var startSession = {startSession: 1};

    // Start up a replica set.
    var dbName = "config";

    var replTest = new ReplSetTest({name: 'refresh', nodes: 3});
    var nodes = replTest.startSet();

    replTest.initiate();
    var primary = replTest.getPrimary();

    replTest.awaitSecondaryNodes();
    var server2 = replTest._slaves[0];
    var server3 = replTest._slaves[1];

    var db1 = primary.getDB(dbName);
    var db2 = server2.getDB(dbName);
    var db3 = server3.getDB(dbName);

    var res;

    // Trigger an initial refresh on all members, as a sanity check.
    res = db1.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");
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

    // Connect to the primary. The sessions collection here should not yet contain records.
    assert.eq(db1.system.sessions.count(), 0, "flushed refresh to the primary prematurely");

    // Trigger a refresh on the primary. The sessions collection should now contain two records.
    res = db1.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");
    assert.eq(
        db1.system.sessions.count(), 2, "should have two local session records after refresh");

    // Trigger another refresh on all members.
    res = db2.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");
    res = db3.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");
    res = db1.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");

    // The sessions collection on the primary should now contain all records.
    assert.eq(
        db1.system.sessions.count(), 3, "should have three local session records after refresh");

    // Stop the test.
    replTest.stopSet();
})();
