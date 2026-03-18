/**
 * Tests that the 'authenticatedMechanism' field on the usersInfo command requires
 * ActionType::internal. External clients should not be able to set this field.
 */

const conn = MongoRunner.runMongod({auth: ""});
const adminDB = conn.getDB("admin");
const testDB = conn.getDB("test");

// Create an admin user with root (but not __system).
adminDB.createUser({user: "admin", pwd: "pwd", roles: ["root"]});
assert(adminDB.auth("admin", "pwd"));

// Create a test user and an admin user that also has the __system role.
testDB.createUser({user: "testUser", pwd: "pwd", roles: []});
adminDB.createUser({user: "system", pwd: "pwd", roles: ["root", "__system"]});

adminDB.logout();

// Authenticate as root (no __system role, so no ActionType::internal).
assert(adminDB.auth("admin", "pwd"));

// A normal usersInfo command should succeed.
assert.commandWorked(testDB.runCommand({usersInfo: "testUser"}));

// usersInfo with authenticatedMechanism should fail because root lacks ActionType::internal.
assert.commandFailedWithCode(
    testDB.runCommand({usersInfo: "testUser", authenticatedMechanism: "SCRAM-SHA-256"}),
    ErrorCodes.Unauthorized,
);

adminDB.logout();

// Now authenticate as a user with the __system role which grants ActionType::internal.
assert(adminDB.auth("system", "pwd"));

// Should succeed with __system role.
assert.commandWorked(testDB.runCommand({usersInfo: "testUser", authenticatedMechanism: "SCRAM-SHA-256"}));

adminDB.logout();
MongoRunner.stopMongod(conn);
