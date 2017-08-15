(function() {
    "use strict";

    var refresh = {refreshLogicalSessionCacheNow: 1};
    var startSession = {startSession: 1};

    // Start up a replica set.
    var dbName = "admin";

    var replTest = new ReplSetTest({name: 'refresh', nodes: 3});
    var nodes = replTest.startSet();

    replTest.initiate();
    var primary = replTest.getPrimary();

    replTest.awaitSecondaryNodes();
    var server2 = replTest.liveNodes.slaves[0];
    var server3 = replTest.liveNodes.slaves[1];

    var admin1 = primary.getDB(dbName);
    var admin2 = server2.getDB(dbName);
    var admin3 = server3.getDB(dbName);

    var res;

    // Trigger an initial refresh on all members, as a sanity check.
    res = admin1.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");
    res = admin2.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");
    res = admin3.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");

    // Connect to the primary and start a session.
    admin1.runCommand(startSession);
    assert.commandWorked(res, "unable to start session");

    // That session should not be in admin.system.sessions yet.
    assert.eq(admin1.system.sessions.count(), 0, "should not have session records yet");

    // Connect to each replica set member and start a session.
    res = admin2.runCommand(startSession);
    assert.commandWorked(res, "unable to start session");
    res = admin3.runCommand(startSession);
    assert.commandWorked(res, "unable to start session");

    // Connect to a secondary and trigger a refresh.
    res = admin2.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");

    // Connect to the primary. The sessions collection here should now contain one record.
    assert.eq(admin1.system.sessions.count(),
              1,
              "refreshing on the secondary did not flush record to the primary");

    // Trigger a refresh on the primary. The sessions collection should now contain two records.
    res = admin1.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");
    assert.eq(
        admin1.system.sessions.count(), 2, "should have two local session records after refresh");

    // Trigger another refresh on all members.
    res = admin1.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");
    res = admin2.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");
    res = admin3.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");

    // The sessions collection on the primary should now contain all records.
    assert.eq(
        admin1.system.sessions.count(), 3, "should have three local session records after refresh");

    // Stop the test.
    replTest.stopSet();
})();
