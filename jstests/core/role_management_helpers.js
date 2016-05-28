// This test is a basic sanity check of the shell helpers for manipulating role objects
// It is not a comprehensive test of the functionality of the role manipulation commands

function assertHasRole(rolesArray, roleName, roleDB) {
    for (i in rolesArray) {
        var curRole = rolesArray[i];
        if (curRole.role == roleName && curRole.db == roleDB) {
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
    assert(false,
           "Privilege " + tojson(privilege) + " not found in privilege array: " +
               tojson(privilegeArray));
}

(function(db) {
    var db = db.getSiblingDB("role_management_helpers");
    db.dropDatabase();
    db.dropAllRoles();

    db.createRole({
        role: 'roleA',
        roles: [],
        privileges: [{resource: {db: db.getName(), collection: "foo"}, actions: ['find']}]
    });
    db.createRole({role: 'roleB', privileges: [], roles: ["roleA"]});
    db.createRole({role: 'roleC', privileges: [], roles: []});

    // Test getRole
    var roleObj = db.getRole("roleA");
    assert.eq(0, roleObj.roles.length);
    assert.eq(null, roleObj.privileges);
    roleObj = db.getRole("roleA", {showPrivileges: true});
    assert.eq(1, roleObj.privileges.length);
    assertHasPrivilege(roleObj.privileges,
                       {resource: {db: db.getName(), collection: "foo"}, actions: ['find']});
    roleObj = db.getRole("roleB", {showPrivileges: true});
    assert.eq(1, roleObj.inheritedPrivileges.length);  // inherited from roleA
    assertHasPrivilege(roleObj.inheritedPrivileges,
                       {resource: {db: db.getName(), collection: "foo"}, actions: ['find']});
    assert.eq(1, roleObj.roles.length);
    assertHasRole(roleObj.roles, "roleA", db.getName());

    // Test getRoles
    var roles = db.getRoles();
    assert.eq(3, roles.length);
    printjson(roles);
    assert(roles[0].role == 'roleA' || roles[1].role == 'roleA' || roles[2].role == 'roleA');
    assert(roles[0].role == 'roleB' || roles[1].role == 'roleB' || roles[2].role == 'roleB');
    assert(roles[0].role == 'roleC' || roles[1].role == 'roleC' || roles[2].role == 'roleC');
    assert.eq(null, roles[0].inheritedPrivileges);
    var roles = db.getRoles({showPrivileges: true, showBuiltinRoles: true});
    assert.eq(9, roles.length);
    assert.neq(null, roles[0].inheritedPrivileges);

    // Granting roles to nonexistent role fails
    assert.throws(function() {
        db.grantRolesToRole("fakeRole", ['dbAdmin']);
    });
    // Granting roles to built-in role fails
    assert.throws(function() {
        db.grantRolesToRole("readWrite", ['dbAdmin']);
    });
    // Granting non-existant role fails
    assert.throws(function() {
        db.grantRolesToRole("roleB", ['dbAdmin', 'fakeRole']);
    });

    roleObj = db.getRole("roleB", {showPrivileges: true});
    assert.eq(1, roleObj.inheritedPrivileges.length);
    assert.eq(1, roleObj.roles.length);
    assertHasRole(roleObj.roles, "roleA", db.getName());

    // Granting a role you already have is no problem
    db.grantRolesToRole("roleB", ['readWrite', 'roleC']);
    roleObj = db.getRole("roleB", {showPrivileges: true});
    assert.gt(roleObj.inheritedPrivileges.length, 1);  // Got privileges from readWrite role
    assert.eq(3, roleObj.roles.length);
    assertHasRole(roleObj.roles, "readWrite", db.getName());
    assertHasRole(roleObj.roles, "roleA", db.getName());
    assertHasRole(roleObj.roles, "roleC", db.getName());

    // Revoking roles the role doesn't have is fine
    db.revokeRolesFromRole("roleB", ['roleA', 'readWrite', 'dbAdmin']);
    roleObj = db.getRole("roleB", {showPrivileges: true});
    assert.eq(0, roleObj.inheritedPrivileges.length);
    assert.eq(1, roleObj.roles.length);
    assertHasRole(roleObj.roles, "roleC", db.getName());

    // Privileges on the same resource get collapsed
    db.grantPrivilegesToRole("roleA", [
        {resource: {db: db.getName(), collection: ""}, actions: ['dropDatabase']},
        {resource: {db: db.getName(), collection: "foo"}, actions: ['insert']}
    ]);
    roleObj = db.getRole("roleA", {showPrivileges: true});
    assert.eq(0, roleObj.roles.length);
    assert.eq(2, roleObj.privileges.length);
    assertHasPrivilege(
        roleObj.privileges,
        {resource: {db: db.getName(), collection: "foo"}, actions: ['find', 'insert']});
    assertHasPrivilege(roleObj.privileges,
                       {resource: {db: db.getName(), collection: ""}, actions: ['dropDatabase']});

    // Update role
    db.updateRole("roleA", {
        roles: ['roleB'],
        privileges: [{resource: {db: db.getName(), collection: "foo"}, actions: ['find']}]
    });
    roleObj = db.getRole("roleA", {showPrivileges: true});
    assert.eq(1, roleObj.roles.length);
    assertHasRole(roleObj.roles, "roleB", db.getName());
    assert.eq(1, roleObj.privileges.length);
    assertHasPrivilege(roleObj.privileges,
                       {resource: {db: db.getName(), collection: "foo"}, actions: ['find']});

    // Test dropRole
    db.dropRole('roleC');
    assert.eq(null, db.getRole('roleC'));
    roleObj = db.getRole("roleB", {showPrivileges: true});
    assert.eq(0, roleObj.privileges.length);
    assert.eq(0, roleObj.roles.length);

    // Test dropAllRoles
    db.dropAllRoles();
    assert.eq(null, db.getRole('roleA'));
    assert.eq(null, db.getRole('roleB'));
    assert.eq(null, db.getRole('roleC'));

}(db));
