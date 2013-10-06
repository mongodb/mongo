// This test is a basic sanity check of the shell helpers for manipulating role objects
// It is not a comprehensive test of the functionality of the role manipulation commands

function assertHasRole(rolesArray, roleName, roleDB) {
    for (i in rolesArray) {
        var curRole = rolesArray[i];
        if (curRole.name == roleName && curRole.db == roleDB) {
            return;
        }
    }
    assert(false, "role " + roleName + "@" + roleDB + " not found in array: " + tojson(rolesArray));
}

function assertHasPrivilege(privilegeArray, privilege) {
    for (i in privilegeArray) {
        var curPriv = privilegeArray[i];
        if (curPriv.resource.cluster == privilege.resource.cluster &&
            curPriv.resource.anyResource == privilege.resource.anyResource &&
            curPriv.resource.db == privilege.resource.db &&
            curPriv.resource.collection == privilege.resource.collection) {
            // Same resource
            assert.eq(curPriv.actions.length, privilege.actions.length);
            for (k in curPriv.actions) {
                assert.eq(curPriv.actions[k], privilege.actions[k]);
            }
            return;
        }
    }
    assert(false, "Privilege " + tojson(privilege) + " not found in privilege array: " +
                   tojson(privilegeArray));
}

(function(db) {
     var db = db.getSiblingDB("role_management_helpers");
     db.dropDatabase();
     db.dropAllRoles();

     db.addRole({name:'roleA', roles: [], privileges: [{resource: {db:db.getName(), collection: ""},
                                                        actions: ['find']}]});
     db.addRole({name:'roleB', privileges: [], roles: ["roleA"]});
     db.addRole({name:'roleC', privileges: [], roles: []});

     // Test getRole
     var roleObj = db.getRole("roleA");
     assert.eq(0, roleObj.roles.length);
     assert.eq(1, roleObj.privileges.length);
     assertHasPrivilege(roleObj.privileges,
                        {resource: {db:db.getName(), collection:""}, actions:['find']});
     roleObj = db.getRole("roleB");
     assert.eq(1, roleObj.privileges.length); // inherited from roleA
     assertHasPrivilege(roleObj.privileges,
                        {resource: {db:db.getName(), collection:""}, actions:['find']});
     assert.eq(1, roleObj.roles.length);
     assertHasRole(roleObj.roles, "roleA", db.getName());

     // Granting roles to nonexistent role fails
     assert.throws(function() { db.grantRolesToRole("fakeRole", ['dbAdmin']); });
     // Granting roles to built-in role fails
     assert.throws(function() { db.grantRolesToRole("readWrite", ['dbAdmin']); });
     // Granting non-existant role fails
     assert.throws(function() { db.grantRolesToRole("roleB", ['dbAdmin', 'fakeRole']); });

     roleObj = db.getRole("roleB");
     assert.eq(1, roleObj.privileges.length);
     assert.eq(1, roleObj.roles.length);
     assertHasRole(roleObj.roles, "roleA", db.getName());

     // Granting a role you already have is no problem
     db.grantRolesToRole("roleB", ['readWrite', 'roleC']);
     roleObj = db.getRole("roleB");
     assert.gt(roleObj.privileges.length, 1); // Got privileges from readWrite role
     assert.eq(3, roleObj.roles.length);
     assertHasRole(roleObj.roles, "readWrite", db.getName());
     assertHasRole(roleObj.roles, "roleA", db.getName());
     assertHasRole(roleObj.roles, "roleC", db.getName());

     // Revoking roles the role doesn't have is fine
     db.revokeRolesFromRole("roleB", ['roleA', 'readWrite', 'dbAdmin']);
     roleObj = db.getRole("roleB");
     assert.eq(0, roleObj.privileges.length);
     assert.eq(1, roleObj.roles.length);
     assertHasRole(roleObj.roles, "roleC", db.getName());

     // Privileges on the same resource get collapsed
     db.grantPrivilegesToRole("roleA",
                              [{resource: {cluster:true}, actions:['listDatabases']},
                               {resource: {db:db.getName(), collection:""}, actions:['insert']}]);
     roleObj = db.getRole("roleA");
     assert.eq(0, roleObj.roles.length);
     assert.eq(2, roleObj.privileges.length);
     assertHasPrivilege(roleObj.privileges,
                        {resource: {db:db.getName(), collection:""}, actions:['find', 'insert']});
     assertHasPrivilege(roleObj.privileges,
                        {resource: {cluster:true}, actions:['listDatabases']});


     // Test dropRole
     db.dropRole('roleC');
     assert.throws(function() {db.getRole('roleC')});
     roleObj = db.getRole("roleB");
     assert.eq(0, roleObj.privileges.length);
     assert.eq(0, roleObj.roles.length);

     // Test dropAllRoles
     db.dropAllRoles();
     assert.throws(function() {db.getRole('roleA')});
     assert.throws(function() {db.getRole('roleB')});
     assert.throws(function() {db.getRole('roleC')});

}(db));