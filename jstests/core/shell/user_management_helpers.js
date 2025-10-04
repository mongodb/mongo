// This test is a basic sanity check of the shell helpers for manipulating user objects
// It is not a comprehensive test of the functionality of the user manipulation commands
//
// @tags: [
//   # The test runs commands that are not allowed with security token: createUser, dropUser,
//   # grantRolesToUser, logout, revokeRolesFromUser, updateUser.
//   not_allowed_with_signed_security_token,
//   assumes_superuser_permissions,
//   assumes_write_concern_unchanged,
//   creates_and_authenticates_user,
//   requires_auth,
//   requires_non_retryable_commands,
// ]

function assertHasRole(rolesArray, roleName, roleDB) {
    for (let i in rolesArray) {
        const curRole = rolesArray[i];
        if (curRole.role == roleName && curRole.db == roleDB) {
            return;
        }
    }
    assert(false, "role " + roleName + "@" + roleDB + " not found in array: " + tojson(rolesArray));
}

function runTest(db) {
    db.dropDatabase();
    db.dropAllUsers();

    db.createUser({user: "spencer", pwd: "password", roles: ["readWrite"]});
    db.createUser({user: "andy", pwd: "password", roles: ["readWrite"]});

    // Test getUser
    let userObj = db.getUser("spencer");
    assert.eq(1, userObj.roles.length);
    assertHasRole(userObj.roles, "readWrite", db.getName());

    // Test getUsers
    let users = db.getUsers();
    assert.eq(2, users.length);
    assert(users[0].user == "spencer" || users[1].user == "spencer");
    assert(users[0].user == "andy" || users[1].user == "andy");
    assert.eq(1, users[0].roles.length);
    assert.eq(1, users[1].roles.length);
    assertHasRole(users[0].roles, "readWrite", db.getName());
    assertHasRole(users[1].roles, "readWrite", db.getName());

    // Granting roles to nonexistent user fails
    assert.throws(function () {
        db.grantRolesToUser("fakeUser", ["dbAdmin"]);
    });
    // Granting non-existant role fails
    assert.throws(function () {
        db.grantRolesToUser("spencer", ["dbAdmin", "fakeRole"]);
    });

    userObj = db.getUser("spencer");
    assert.eq(1, userObj.roles.length);
    assertHasRole(userObj.roles, "readWrite", db.getName());

    // Granting a role you already have is no problem
    db.grantRolesToUser("spencer", ["readWrite", "dbAdmin"]);
    userObj = db.getUser("spencer");
    assert.eq(2, userObj.roles.length);
    assertHasRole(userObj.roles, "readWrite", db.getName());
    assertHasRole(userObj.roles, "dbAdmin", db.getName());

    // Revoking roles the user doesn't have is fine
    db.revokeRolesFromUser("spencer", ["dbAdmin", "read"]);
    userObj = db.getUser("spencer");
    assert.eq(1, userObj.roles.length);
    assertHasRole(userObj.roles, "readWrite", db.getName());

    // Update user
    db.updateUser("spencer", {customData: {hello: "world"}, roles: ["read"]});
    userObj = db.getUser("spencer");
    assert.eq("world", userObj.customData.hello);
    assert.eq(1, userObj.roles.length);
    assertHasRole(userObj.roles, "read", db.getName());

    // Test dropUser
    db.dropUser("andy");
    assert.eq(null, db.getUser("andy"));

    // Test dropAllUsers
    db.dropAllUsers();
    assert.eq(0, db.getUsers().length);

    // Test password digestion
    assert.throws(function () {
        db.createUser({user: "user1", pwd: "x", roles: [], digestPassword: true});
    });
    assert.throws(function () {
        db.createUser({user: "user1", pwd: "x", roles: [], digestPassword: false});
    });
    assert.throws(function () {
        db.createUser({user: "user1", pwd: "x", roles: [], passwordDigestor: "foo"});
    });
    db.createUser({user: "user1", pwd: "x", roles: [], passwordDigestor: "server"});
    assert(db.auth("user1", "x"));
    assert.throws(function () {
        db.updateUser("user1", {pwd: "y", digestPassword: true});
    });
    assert.throws(function () {
        db.updateUser("user1", {pwd: "y", digestPassword: false});
    });
    assert.throws(function () {
        db.updateUser("user1", {pwd: "y", passwordDigestor: "foo"});
    });

    // Change password and reauth using new credentials.
    db.updateUser("user1", {pwd: "y", passwordDigestor: "server"});
    assert(db.auth("user1", "y"));
    db.logout();

    // Note that as of SERVER-32974, client-side digestion is only permitted under the SCRAM-SHA-1
    // mechanism.
    db.createUser({
        user: "user2",
        pwd: "x",
        roles: [],
        mechanisms: ["SCRAM-SHA-1"],
        passwordDigestor: "client",
    });
    assert(db.auth("user2", "x"));
    db.updateUser("user2", {pwd: "y", mechanisms: ["SCRAM-SHA-1"], passwordDigestor: "client"});
    assert(db.auth("user2", "y"));
    db.logout();

    // Test createUser requires 'user' field
    assert.throws(function () {
        db.createUser({pwd: "x", roles: ["dbAdmin"]});
    });

    // Test createUser disallows 'createUser' field
    assert.throws(function () {
        db.createUser({createUser: "ben", pwd: "x", roles: ["dbAdmin"]});
    });
}

try {
    runTest(db.getSiblingDB("user_management_helpers"));
} catch (x) {
    // BF-836 Print current users on failure to aid debugging
    db.getSiblingDB("admin").system.users.find().forEach(printjson);
    throw x;
}
