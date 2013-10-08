// This test is a basic sanity check of the shell helpers for manipulating user objects
// It is not a comprehensive test of the functionality of the user manipulation commands
function assertHasRole(rolesArray, roleName, roleDB) {
    for (i in rolesArray) {
        var curRole = rolesArray[i];
        if (curRole.name == roleName && curRole.db == roleDB) {
            return;
        }
    }
    assert(false, "role " + roleName + "@" + roleDB + " not found in array: " + tojson(rolesArray));
}


(function(db) {
     var db = db.getSiblingDB("user_management_helpers");
     db.dropDatabase();
     db.dropAllUsers();

     db.addUser("spencer", "password", ['readWrite']);
     db.addUser("andy", "password", ['readWrite']);

     // Test getUser
     var userObj = db.getUser('spencer');
     assert.eq(1, userObj.roles.length);
     assertHasRole(userObj.roles, "readWrite", db.getName());

     // Test getUsers
     var users = db.getUsers();
     assert.eq(2, users.length);
     assert(users[0].name == 'spencer' || users[1].name == 'spencer');
     assert(users[0].name == 'andy' || users[1].name == 'andy');
     assert.eq(1, users[0].roles.length);
     assert.eq(1, users[1].roles.length);
     assertHasRole(users[0].roles, "readWrite", db.getName());
     assertHasRole(users[1].roles, "readWrite", db.getName());

     // Granting roles to nonexistent user fails
     assert.throws(function() { db.grantRolesToUser("fakeUser", ['dbAdmin']); });
     // Granting non-existant role fails
     assert.throws(function() { db.grantRolesToUser("spencer", ['dbAdmin', 'fakeRole']); });

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
     db.revokeRolesFromUser("spencer", ['dbAdmin', 'read'])
     userObj = db.getUser('spencer');
     assert.eq(1, userObj.roles.length);
     assertHasRole(userObj.roles, "readWrite", db.getName());

     // Test dropUser
     db.dropUser('andy');
     assert.throws(function() {printjson(db.getUser('andy'))});

     // Test dropAllUsers
     db.dropAllUsers()
     assert.eq(0, db.getUsers().length);
}(db));