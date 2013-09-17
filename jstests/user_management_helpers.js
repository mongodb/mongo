function assertHasRole(rolesArray, roleName, roleSource, hasRole, canDelegate) {
    for (i in rolesArray) {
        var curRole = rolesArray[i];
        if (curRole.name == roleName && curRole.source == roleSource) {
            assert.eq(hasRole, curRole.hasRole);
            assert.eq(canDelegate, curRole.canDelegate);
            return;
        }
    }
    assert(false, "role " + roleName + "@" + roleSource + " not found in array: " + rolesArray);
}



db = db.getSiblingDB("user_management_helpers");
db.dropDatabase();
db.removeAllUsers();

db.addUser("spencer", "password", ['readWrite']);
db.addUser("andy", "password", ['readWrite']);

// Test getUser
var userObj = db.getUser('spencer');
assert.eq(1, userObj.roles.length);
assertHasRole(userObj.roles, "readWrite", db.getName(), true, false);

// Test getUsers
var users = db.getUsers();
assert.eq(2, users.length);
assert(users[0].name == 'spencer' || users[1].name == 'spencer');
assert(users[0].name == 'andy' || users[1].name == 'andy');
assert.eq(1, users[0].roles.length);
assert.eq(1, users[1].roles.length);
assertHasRole(users[0].roles, "readWrite", db.getName(), true, false);
assertHasRole(users[1].roles, "readWrite", db.getName(), true, false);


// Granting roles to nonexistent user fails
assert.throws(function() { db.grantRolesToUser("fakeUser", ['dbAdmin']); });
// Granting non-existant role fails
assert.throws(function() { db.grantRolesToUser("spencer", ['dbAdmin', 'fakeRole']); });

userObj = db.getUser('spencer');
assert.eq(1, userObj.roles.length);
assertHasRole(userObj.roles, "readWrite", db.getName(), true, false);

// Granting a role you already have is no problem
db.grantRolesToUser("spencer", ['readWrite', 'dbAdmin']);
userObj = db.getUser('spencer');
assert.eq(2, userObj.roles.length);
assertHasRole(userObj.roles, "readWrite", db.getName(), true, false);
assertHasRole(userObj.roles, "dbAdmin", db.getName(), true, false);

// Grant delgation
db.grantDelegateRolesToUser("spencer", ['readWrite', 'read']);
userObj = db.getUser('spencer');
assert.eq(3, userObj.roles.length);
assertHasRole(userObj.roles, "readWrite", db.getName(), true, true);
assertHasRole(userObj.roles, "dbAdmin", db.getName(), true, false);
assertHasRole(userObj.roles, "read", db.getName(), false, true);

// read role is unaffected b/c user only had delegate on it, dbAdmin role is completely removed
db.revokeRolesFromUser("spencer", ['readWrite', 'dbAdmin', 'read'])
userObj = db.getUser('spencer');
assert.eq(2, userObj.roles.length);
assertHasRole(userObj.roles, "readWrite", db.getName(), false, true);
assertHasRole(userObj.roles, "read", db.getName(), false, true);

// Revoking roles the user doesn't have is fine
db.revokeDelegateRolesFromUser('spencer', ['read', 'dbAdmin'])
userObj = db.getUser('spencer');
assert.eq(1, userObj.roles.length);
assertHasRole(userObj.roles, "readWrite", db.getName(), false, true);


