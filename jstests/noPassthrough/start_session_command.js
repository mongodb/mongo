(function() {
    'use strict';
    var conn;
    var admin;
    var foo;
    var result;
    const request = {startSession: 1};

    conn = MongoRunner.runMongod({nojournal: ""});
    admin = conn.getDB("admin");

    // ensure that the cache is empty
    var serverStatus = assert.commandWorked(admin.adminCommand({serverStatus: 1}));
    assert.eq(0, serverStatus.logicalSessionRecordCache.activeSessionsCount);

    // test that we can run startSession unauthenticated when the server is running without --auth

    result = admin.runCommand(request);
    assert.commandWorked(
        result,
        "failed test that we can run startSession unauthenticated when the server is running without --auth");
    assert(result.id, "failed test that our session response has an id");
    assert.eq(
        result.timeoutMinutes, 30, "failed test that our session record has the correct timeout");

    // test that startSession added to the cache
    serverStatus = assert.commandWorked(admin.adminCommand({serverStatus: 1}));
    assert.eq(1, serverStatus.logicalSessionRecordCache.activeSessionsCount);

    // test that we can run startSession authenticated when the server is running without --auth

    admin.createUser({user: 'user0', pwd: 'password', roles: []});
    admin.auth("user0", "password");

    result = admin.runCommand(request);
    assert.commandWorked(
        result,
        "failed test that we can run startSession authenticated when the server is running without --auth");
    assert(result.id, "failed test that our session response has an id");
    assert.eq(
        result.timeoutMinutes, 30, "failed test that our session record has the correct timeout");

    MongoRunner.stopMongod(conn);

    //

    conn = MongoRunner.runMongod({auth: "", nojournal: ""});
    admin = conn.getDB("admin");
    foo = conn.getDB("foo");

    // test that we can't run startSession unauthenticated when the server is running with --auth

    assert.commandFailed(
        admin.runCommand(request),
        "failed test that we can't run startSession unauthenticated when the server is running with --auth");

    //

    admin.createUser({user: 'admin', pwd: 'admin', roles: jsTest.adminUserRoles});
    admin.auth("admin", "admin");
    admin.createUser({user: 'user0', pwd: 'password', roles: jsTest.basicUserRoles});
    foo.createUser({user: 'user1', pwd: 'password', roles: jsTest.basicUserRoles});
    admin.createUser({user: 'user2', pwd: 'password', roles: []});
    admin.logout();

    // test that we can run startSession authenticated as one user with proper permissions

    admin.auth("user0", "password");
    result = admin.runCommand(request);
    assert.commandWorked(
        result,
        "failed test that we can run startSession authenticated as one user with proper permissions");
    assert(result.id, "failed test that our session response has an id");
    assert.eq(
        result.timeoutMinutes, 30, "failed test that our session record has the correct timeout");

    // test that we cant run startSession authenticated as two users with proper permissions

    foo.auth("user1", "password");
    assert.commandFailed(
        admin.runCommand(request),
        "failed test that we cant run startSession authenticated as two users with proper permissions");

    // test that we cant run startSession authenticated as one user without proper permissions

    admin.logout();
    admin.auth("user2", "password");
    assert.commandFailed(
        admin.runCommand(request),
        "failed test that we cant run startSession authenticated as one user without proper permissions");

    //

    MongoRunner.stopMongod(conn);

})();
