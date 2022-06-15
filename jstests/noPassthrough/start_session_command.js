(function() {
'use strict';

// This test makes assertions about the number of sessions, which are not compatible with
// implicit sessions.
TestData.disableImplicitSessions = true;

const request = {
    startSession: 1
};

let conn = MongoRunner.runMongod({setParameter: {maxSessions: 2}});
let admin = conn.getDB("admin");

// ensure that the cache is empty
let serverStatus = assert.commandWorked(admin.adminCommand({serverStatus: 1}));
assert.eq(0, serverStatus.logicalSessionRecordCache.activeSessionsCount);

// test that we can run startSession unauthenticated when the server is running without --auth

let result = admin.runCommand(request);
assert.commandWorked(
    result,
    "failed test that we can run startSession unauthenticated when the server is running without --auth");
assert(result.id, "failed test that our session response has an id");
assert.eq(result.timeoutMinutes, 30, "failed test that our session record has the correct timeout");

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
assert.eq(result.timeoutMinutes, 30, "failed test that our session record has the correct timeout");

assert.commandFailed(admin.runCommand(request),
                     "failed test that we can't run startSession when the cache is full");
MongoRunner.stopMongod(conn);

//

conn = MongoRunner.runMongod({auth: ""});
admin = conn.getDB("admin");

// test that we can't run startSession unauthenticated when the server is running with --auth

assert.commandFailed(
    admin.runCommand(request),
    "failed test that we can't run startSession unauthenticated when the server is running with --auth");

//

admin.createUser({user: 'admin', pwd: 'admin', roles: jsTest.adminUserRoles});
admin.auth("admin", "admin");
admin.createUser({user: 'user0', pwd: 'password', roles: jsTest.basicUserRoles});
admin.logout();

// test that we can run startSession authenticated as one user with proper permissions

admin.auth("user0", "password");
result = admin.runCommand(request);
assert.commandWorked(
    result,
    "failed test that we can run startSession authenticated as one user with proper permissions");
assert(result.id, "failed test that our session response has an id");
assert.eq(result.timeoutMinutes, 30, "failed test that our session record has the correct timeout");
admin.logout();

//

MongoRunner.stopMongod(conn);
})();
