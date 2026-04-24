/**
 * Tests that privilege changes on roles and users take effect immediately for
 * already-authenticated sessions, without requiring re-authentication.
 */

const conn = MongoRunner.runMongod({auth: ""});

// Bootstrap root and a userAdmin for role/user manipulation.
const adminDB = conn.getDB("admin");
adminDB.createUser({user: "root", pwd: "root", roles: ["root"]});
adminDB.auth("root", "root");
adminDB.createUser({user: "userAdmin", pwd: "pwd", roles: ["userAdminAnyDatabase"]});
assert.commandWorked(conn.getDB("test").foo.insert({x: 1}));
adminDB.logout();

// Separate connection mutates roles/users while testDB is the session under test.
const adminConn = new Mongo(conn.host);
const adminTestDB = adminConn.getDB("test");
adminConn.getDB("admin").auth("userAdmin", "pwd");

adminTestDB.createRole({role: "liveRole", privileges: [], roles: []});
adminTestDB.createUser({user: "testUser", pwd: "pwd", roles: ["liveRole"]});

const testDB = conn.getDB("test");
assert(testDB.auth("testUser", "pwd"));

const findPriv = [{resource: {db: "test", collection: ""}, actions: ["find"]}];
function assertFind(allowed) {
    const res = testDB.runCommand({find: "foo"});
    if (allowed) {
        assert.commandWorked(res);
    } else {
        assert.commandFailedWithCode(res, ErrorCodes.Unauthorized);
    }
}

// Grant/revoke privilege on a held role propagates to the live session.
assertFind(false);
adminTestDB.grantPrivilegesToRole("liveRole", findPriv);
assertFind(true);
adminTestDB.revokePrivilegesFromRole("liveRole", findPriv);
assertFind(false);

// Granting a built-in role directly to the user also propagates.
adminTestDB.grantRolesToUser("testUser", ["read"]);
assertFind(true);

// Revoking all roles leaves the user authenticated but without privileges.
adminTestDB.revokeRolesFromUser("testUser", ["read", "liveRole"]);
const status = assert.commandWorked(testDB.runCommand({connectionStatus: 1}));
assert.eq("testUser", status.authInfo.authenticatedUsers[0].user);
assertFind(false);
assert.commandFailedWithCode(testDB.runCommand({insert: "foo", documents: [{y: 1}]}), ErrorCodes.Unauthorized);

// Dropping a held role revokes its privileges from the live session.
adminTestDB.grantRolesToUser("testUser", ["liveRole"]);
adminTestDB.grantPrivilegesToRole("liveRole", findPriv);
assertFind(true);
adminTestDB.dropRole("liveRole");
assertFind(false);

// updateUser replacing roles propagates immediately.
adminTestDB.updateUser("testUser", {roles: ["readWrite"]});
assertFind(true);
assert.commandWorked(testDB.foo.insert({z: 1}));
adminTestDB.updateUser("testUser", {roles: []});
assertFind(false);

MongoRunner.stopMongod(conn);
