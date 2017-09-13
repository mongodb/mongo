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

    MongoRunner.stopMongod(conn);
})();
