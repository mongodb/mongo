'use strict';

// Make sure that listCommands doesn't require authentication.
function runTest(conn) {
    const admin = conn.getDB('admin');

    // Commands should succeed in auth-bypass mode regardless of requiresAuth().
    assert.commandWorked(admin.runCommand({listDatabases: 1}),
                         "listDatabases shouldn't work pre-auth");
    assert.commandWorked(admin.runCommand({listCommands: 1}), "listCommands should work pre-auth");

    admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});

    // listDatabases should now fail, because auth bypass is no longer valid.
    assert.commandFailed(admin.runCommand({listDatabases: 1}),
                         "listDatabases shouldn't work pre-auth");
    // listCommands should STILL work, because it does not require auth.
    assert.commandWorked(admin.runCommand({listCommands: 1}), "listCommands should work pre-auth");
}
