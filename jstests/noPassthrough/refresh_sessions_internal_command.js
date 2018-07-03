(function() {
    "use strict";

    var conn;
    var admin;

    conn = MongoRunner.runMongod({auth: "", nojournal: ""});
    admin = conn.getDB("admin");

    admin.createUser({user: 'admin', pwd: 'admin', roles: jsTest.adminUserRoles});
    admin.auth("admin", "admin");

    result = admin.runCommand({
        createRole: 'impersonate',
        privileges: [{resource: {cluster: true}, actions: ['impersonate']}],
        roles: []
    });
    assert.commandWorked(result, "couldn't make impersonate role");

    admin.createUser({user: 'internal', pwd: 'pwd', roles: ['impersonate']});

    // Test that we cannot run refreshSessions unauthenticated if --auth is on.
    var result = admin.runCommand({refreshSessionsInternal: []});
    assert.commandFailed(result, "able to run refreshSessionsInternal without authenticating");

    // Test that we cannot run refreshSessionsInternal without impersonate privileges.
    admin.auth("admin", "admin");
    result = admin.runCommand({refreshSessionsInternal: []});
    assert.commandFailed(result, "able to run refreshSessions without impersonate privileges");
    admin.logout();

    // Test that we can run refreshSessionsInternal if we can impersonate.
    admin.auth("internal", "pwd");
    result = admin.runCommand({refreshSessionsInternal: []});
    assert.commandWorked(result, "unable to run command with impersonate privileges");
    admin.logout();

    MongoRunner.stopMongod(conn);

    TestData.disableImplicitSessions = true;

    // Start sessions and save the logical session record objects
    var refresh = {refreshLogicalSessionCacheNow: 1};
    var startSession = {startSession: 1};

    conn = MongoRunner.runMongod();
    admin = conn.getDB("admin");
    var config = conn.getDB("config");

    for (var i = 0; i < 3; i++) {
        result = admin.runCommand(startSession);
        assert.commandWorked(result, "unable to start session");
    }
    result = admin.runCommand(refresh);
    assert.commandWorked(result, "failed to refresh");
    var sessions = config.system.sessions.find().toArray();
    assert.eq(sessions.length, 3, "refresh should have written three session records");

    MongoRunner.stopMongod(conn);

    // Test that we can run refreshSessionsInternal with logical session record objects
    conn = MongoRunner.runMongod({setParameter: {maxSessions: 2}});
    admin = conn.getDB("admin");

    result = admin.runCommand({refreshSessionsInternal: sessions.slice(0, 2)});
    assert.commandWorked(result,
                         "unable to run refreshSessionsInternal when the cache is not full");

    result = admin.runCommand({refreshSessionsInternal: sessions.slice(2)});
    assert.commandFailed(result, "able to run refreshSessionsInternal when the cache is full");

    MongoRunner.stopMongod(conn);
})();
