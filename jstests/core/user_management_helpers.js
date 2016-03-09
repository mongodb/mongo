// This test is a basic sanity check of the shell helpers for manipulating user objects
// It is not a comprehensive test of the functionality of the user manipulation commands
function assertHasRole(rolesArray, roleName, roleDB) {
    for (i in rolesArray) {
        var curRole = rolesArray[i];
        if (curRole.role == roleName && curRole.db == roleDB) {
            return;
        }
    }
    assert(false, "role " + roleName + "@" + roleDB + " not found in array: " + tojson(rolesArray));
}

function runTest(db) {
    var db = db.getSiblingDB("user_management_helpers");
    db.dropDatabase();
    db.dropAllUsers();

    db.createUser({user: "spencer", pwd: "password", roles: ['readWrite']});
    db.createUser({user: "andy", pwd: "password", roles: ['readWrite']});

    // Test getUser
    var userObj = db.getUser('spencer');
    assert.eq(1, userObj.roles.length);
    assertHasRole(userObj.roles, "readWrite", db.getName());

    // Test getUsers
    var users = db.getUsers();
    assert.eq(2, users.length);
    assert(users[0].user == 'spencer' || users[1].user == 'spencer');
    assert(users[0].user == 'andy' || users[1].user == 'andy');
    assert.eq(1, users[0].roles.length);
    assert.eq(1, users[1].roles.length);
    assertHasRole(users[0].roles, "readWrite", db.getName());
    assertHasRole(users[1].roles, "readWrite", db.getName());

    // Granting roles to nonexistent user fails
    assert.throws(function() {
        db.grantRolesToUser("fakeUser", ['dbAdmin']);
    });
    // Granting non-existant role fails
    assert.throws(function() {
        db.grantRolesToUser("spencer", ['dbAdmin', 'fakeRole']);
    });

    userObj = db.getUser('spencer');
    assert.eq(1, userObj.roles.length);
    assertHasRole(userObj.roles, "readWrite", db.getName());

    // Granting a role you already have is no problem
    db.grantRolesToUser("spencer", ['readWrite', 'dbAdmin']);
    userObj = db.getUser('spencer');
    assert.eq(2, userObj.roles.length);
    assertHasRole(userObj.roles, "readWrite", db.getName());
    assertHasRole(userObj.roles, "dbAdmin", db.getName());

    // Revoking roles the user doesn't have is fine
    db.revokeRolesFromUser("spencer", ['dbAdmin', 'read']);
    userObj = db.getUser('spencer');
    assert.eq(1, userObj.roles.length);
    assertHasRole(userObj.roles, "readWrite", db.getName());

    // Update user
    db.updateUser("spencer", {customData: {hello: 'world'}, roles: ['read']});
    userObj = db.getUser('spencer');
    assert.eq('world', userObj.customData.hello);
    assert.eq(1, userObj.roles.length);
    assertHasRole(userObj.roles, "read", db.getName());

    // Test dropUser
    db.dropUser('andy');
    assert.eq(null, db.getUser('andy'));

    // Test dropAllUsers
    db.dropAllUsers();
    assert.eq(0, db.getUsers().length);

    // Test password digestion
    assert.throws(function() {
        db.createUser({user: 'user1', pwd: 'x', roles: [], digestPassword: true});
    });
    assert.throws(function() {
        db.createUser({user: 'user1', pwd: 'x', roles: [], digestPassword: false});
    });
    assert.throws(function() {
        db.createUser({user: 'user1', pwd: 'x', roles: [], passwordDigestor: 'foo'});
    });
    db.createUser({user: 'user1', pwd: 'x', roles: [], passwordDigestor: "server"});
    db.createUser({user: 'user2', pwd: 'x', roles: [], passwordDigestor: "client"});
    assert(db.auth('user1', 'x'));
    assert(db.auth('user2', 'x'));

    assert.throws(function() {
        db.updateUser('user1', {pwd: 'y', digestPassword: true});
    });
    assert.throws(function() {
        db.updateUser('user1', {pwd: 'y', digestPassword: false});
    });
    assert.throws(function() {
        db.updateUser('user1', {pwd: 'y', passwordDigestor: 'foo'});
    });
    db.updateUser('user1', {pwd: 'y', passwordDigestor: 'server'});
    db.updateUser('user2', {pwd: 'y', passwordDigestor: 'client'});
    assert(db.auth('user1', 'y'));
    assert(db.auth('user2', 'y'));
}

try {
    runTest(db);
} catch (x) {
    // BF-836 Print current users on failure to aid debugging
    db.getSiblingDB('admin').system.users.find().forEach(printjson);
    throw x;
}