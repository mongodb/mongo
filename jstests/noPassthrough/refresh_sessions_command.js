(function() {
    "use strict";

    var conn;
    var admin;
    var result;
    var startSession = {startSession: 1};

    // Run initial tests without auth.
    conn = MongoRunner.runMongod({nojournal: ""});
    admin = conn.getDB("admin");

    result = admin.runCommand(startSession);
    assert.commandWorked(result, "failed to startSession");
    var lsid = result.id;

    // Test that we can run refreshSessions unauthenticated if --auth is off.
    result = admin.runCommand({refreshSessions: [lsid]});
    assert.commandWorked(result, "could not run refreshSessions unauthenticated without --auth");

    // Test that we can run refreshSessions authenticated if --auth is off.
    admin.createUser(
        {user: 'admin', pwd: 'admin', roles: ['readAnyDatabase', 'userAdminAnyDatabase']});
    admin.auth("admin", "admin");
    result = admin.runCommand(startSession);
    var lsid2 = result.id;
    result = admin.runCommand({refreshSessions: [lsid2]});
    assert.commandWorked(result, "could not run refreshSessions logged in with --auth off");

    // Turn on auth for further testing.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({auth: "", nojournal: ""});
    admin = conn.getDB("admin");

    admin.createUser(
        {user: 'admin', pwd: 'admin', roles: ['readAnyDatabase', 'userAdminAnyDatabase']});
    admin.auth("admin", "admin");

    result = admin.runCommand({
        createRole: 'readSessionsCollection',
        privileges: [{resource: {db: 'config', collection: 'system.sessions'}, actions: ['find']}],
        roles: []
    });
    assert.commandWorked(result, "couldn't make readSessionsCollection role");

    admin.createUser(
        {user: 'readSessionsCollection', pwd: 'pwd', roles: ['readSessionsCollection']});
    admin.logout();

    // Test that we cannot run refreshSessions unauthenticated if --auth is on.
    result = admin.runCommand({refreshSessions: [lsid]});
    assert.commandFailed(result, "able to run refreshSessions without authenticating");

    // Test that we can run refreshSessions on our own sessions authenticated if --auth is on.
    admin.auth("admin", "admin");
    result = admin.runCommand(startSession);
    var lsid3 = result.id;
    result = admin.runCommand({refreshSessions: [lsid3]});
    assert.commandWorked(result, "unable to run refreshSessions while logged in");

    // Test that we can refresh "others'" sessions (new ones) when authenticated with --auth.
    result = admin.runCommand({refreshSessions: [lsid]});
    assert.commandWorked(result, "unable to refresh novel lsids");

    // Test that sending a mix of known and new sessions is fine
    result = admin.runCommand({refreshSessions: [lsid, lsid2, lsid3]});
    assert.commandWorked(result, "unable to refresh mix of known and unknown lsids");

    // Test that sending a set of sessions with duplicates is fine
    result = admin.runCommand({refreshSessions: [lsid, lsid, lsid, lsid]});
    assert.commandWorked(result, "unable to refresh with duplicate lsids in the set");

    // Test that we can run refreshSessions with an empty set of sessions.
    result = admin.runCommand({refreshSessions: []});
    assert.commandWorked(result, "unable to refresh empty set of lsids");

    // Test that once we force a refresh, all of these sessions are in the sessions collection.
    admin.logout();
    admin.auth("readSessionsCollection", "pwd");
    result = admin.runCommand({refreshLogicalSessionCacheNow: 1});
    assert.commandWorked(result, "could not force refresh");

    var config = conn.getDB("config");
    assert.eq(config.system.sessions.count(), 3, "should have refreshed all session records");

    MongoRunner.stopMongod(conn);
})();
