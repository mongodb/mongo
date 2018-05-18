(function() {
    "use script";

    // This test makes assertions about the number of sessions, which are not compatible with
    // implicit sessions.
    TestData.disableImplicitSessions = true;

    var res;
    var refresh = {refreshLogicalSessionCacheNow: 1};
    var startSession = {startSession: 1};

    // Start up a standalone server.
    var conn = MongoRunner.runMongod({nojournal: ""});
    var admin = conn.getDB("admin");
    var config = conn.getDB("config");

    // Trigger an initial refresh, as a sanity check.
    res = admin.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");

    var sessions = [];
    for (var i = 0; i < 20; i++) {
        res = admin.runCommand(startSession);
        assert.commandWorked(res, "unable to start session");
        sessions.push(res);
    }

    res = admin.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");

    assert.eq(config.system.sessions.count(), 20, "refresh should have written 20 session records");

    var endSessionsIds = [];
    for (var i = 0; i < 10; i++) {
        endSessionsIds.push(sessions[i].id);
    }
    res = admin.runCommand({endSessions: endSessionsIds});
    assert.commandWorked(res, "failed to end sessions");

    res = admin.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");

    assert.eq(config.system.sessions.count(),
              10,
              "endSessions and refresh should result in 10 remaining sessions");

    // double delete the remaining 10
    endSessionsIds = [];
    for (var i = 10; i < 20; i++) {
        endSessionsIds.push(sessions[i].id);
        endSessionsIds.push(sessions[i].id);
    }

    res = admin.runCommand({endSessions: endSessionsIds});
    assert.commandWorked(res, "failed to end sessions");

    res = admin.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");

    assert.eq(config.system.sessions.count(),
              0,
              "endSessions and refresh should result in 0 remaining sessions");

    // delete some sessions that were never created
    res = admin.runCommand({
        endSessions: [
            {"id": UUID("bacb219c-214c-47f9-a94a-6c7f434b3bae")},
            {"id": UUID("bacb219c-214c-47f9-a94a-6c7f434b3baf")}
        ]
    });

    res = admin.runCommand(refresh);
    assert.commandWorked(res, "failed to refresh");

    // verify that end on the session handle actually ends sessions
    {
        var session = conn.startSession();

        assert.commandWorked(session.getDatabase("admin").runCommand({usersInfo: 1}),
                             "do something to tickle the session");
        assert.commandWorked(session.getDatabase("admin").runCommand(refresh), "failed to refresh");
        assert.eq(
            config.system.sessions.count(), 1, "usersInfo should have written 1 session record");

        session.endSession();
        assert.commandWorked(admin.runCommand(refresh), "failed to refresh");
        assert.eq(config.system.sessions.count(),
                  0,
                  "endSessions and refresh should result in 0 remaining sessions");
    }

    MongoRunner.stopMongod(conn);
}());
