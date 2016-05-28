/**
 * This tests that all the different commands for role manipulation all properly handle invalid
 * and atypical inputs.
 */

function runTest(conn) {
    var db = conn.getDB('test');
    var admin = conn.getDB('admin');
    admin.createUser({user: 'userAdmin', pwd: 'pwd', roles: ['userAdminAnyDatabase']});
    admin.auth('userAdmin', 'pwd');

    (function testCreateRole() {
        jsTestLog("Testing createRole");

        // Role with no privs
        db.createRole({role: "role1", roles: [], privileges: []});

        // Role with duplicate other roles
        db.createRole({role: "role2", roles: ['read', 'read', 'role1', 'role1'], privileges: []});
        assert.eq(2, db.getRole("role2").roles.length);

        // Role with duplicate privileges
        db.createRole({
            role: "role3",
            roles: ['role2'],
            privileges: [
                {resource: {db: 'test', collection: ''}, actions: ['find']},
                {resource: {db: 'test', collection: ''}, actions: ['find']}
            ]
        });
        assert.eq(1, db.getRole("role3", {showPrivileges: true}).privileges.length);

        // Try to create role that already exists.
        assert.throws(function() {
            db.createRole({role: 'role2', roles: [], privileges: []});
        });

        // Try to create role with no name
        assert.throws(function() {
            db.createRole({role: '', roles: [], privileges: []});
        });

        // Try to create a role the wrong types for the arguments
        assert.throws(function() {
            db.createRole({role: 1, roles: [], privileges: []});
        });
        assert.throws(function() {
            db.createRole({role: ["role4", "role5"], roles: [], privileges: []});
        });
        assert.throws(function() {
            db.createRole({role: 'role6', roles: 1, privileges: []});
        });
        assert.throws(function() {
            db.createRole({role: 'role7', roles: [], privileges: 1});
        });

        // Try to create a role with an invalid privilege
        assert.throws(function() {
            db.createRole(
                {role: 'role8', roles: [], privileges: [{resource: {}, actions: ['find']}]});
        });
        assert.throws(function() {
            db.createRole({
                role: 'role9',
                roles: [],
                privileges: [{resource: {db: "test", collection: "foo"}, actions: []}]
            });
        });
        assert.throws(function() {
            db.createRole({
                role: 'role10',
                roles: [],
                privileges: [{resource: {db: "test"}, actions: ['find']}]
            });
        });
        assert.throws(function() {
            db.createRole({
                role: 'role11',
                roles: [],
                privileges: [{resource: {collection: "foo"}, actions: ['find']}]
            });
        });
        assert.throws(function() {
            db.createRole({
                role: 'role12',
                roles: [],
                privileges: [{resource: {anyResource: false}, actions: ['find']}]
            });
        });
        assert.throws(function() {
            db.createRole({
                role: 'role13',
                roles: [],
                privileges: [{
                    resource: {db: "test", collection: "foo", cluster: true},
                    actions: ['find']
                }]
            });
        });
        assert.throws(function() {
            db.createRole({
                role: 'role14',
                roles: [],
                privileges: [{resource: {cluster: false}, actions: ['find']}]
            });
        });
        assert.throws(function() {
            db.createRole({
                role: 'role15',
                roles: [],
                privileges: [{resource: {db: "test", collection: "$cmd"}, actions: ['find']}]
            });
        });
        assert.throws(function() {
            db.createRole({
                role: 'role16',
                roles: [],
                privileges:
                    [{resource: {db: "test", collection: "foo"}, actions: ['fakeAction']}]
            });
        });

        // Try to create role containing itself in its roles array
        assert.throws(function() {
            db.createRole({role: 'role17', roles: ['role10'], privileges: []});
        });

        assert.eq(3, db.getRoles().length);
    })();

    (function testUpdateRole() {
        jsTestLog("Testing updateRole");

        // Try to update role that doesn't exist
        assert.throws(function() {
            db.updateRole("fakeRole", {roles: []});
        });

        // Try to update role to have a role that doesn't exist
        assert.throws(function() {
            db.updateRole("role1", {roles: ['fakeRole']});
        });

        // Try to update a built-in role
        assert.throws(function() {
            db.updateRole("read", {roles: ['readWrite']});
        });

        // Try to create a cycle in the role graph
        assert.throws(function() {
            db.updateRole("role1", {roles: ['role1']});
        });
        assert.eq(0, db.getRole('role1').roles.length);

        assert.throws(function() {
            db.updateRole("role1", {roles: ['role2']});
        });
        assert.eq(0, db.getRole('role1').roles.length);

        assert.throws(function() {
            db.updateRole("role1", {roles: ['role3']});
        });
        assert.eq(0, db.getRole('role1').roles.length);
    })();

    (function testGrantRolesToRole() {
        jsTestLog("Testing grantRolesToRole");

        // Grant role1 to role2 even though role2 already has role1
        db.grantRolesToRole("role2", ['role1']);
        assert.eq(2, db.getRole('role2').roles.length);

        // Try to grant a role that doesn't exist
        assert.throws(function() {
            db.grantRolesToRole("role1", ['fakeRole']);
        });

        // Try to grant *to* a role that doesn't exist
        assert.throws(function() {
            db.grantRolesToRole("fakeRole", ['role1']);
        });

        // Must specify at least 1 role
        assert.throws(function() {
            db.grantRolesToRole("role1", []);
        });

        // Try to grant to a built-in role
        assert.throws(function() {
            db.grantRolesToRole("read", ['role1']);
        });

        // Try to create a cycle in the role graph
        assert.throws(function() {
            db.grantRolesToRole("role1", ['role1']);
        });
        assert.eq(0, db.getRole('role1').roles.length);

        assert.throws(function() {
            db.grantRolesToRole("role1", ['role2']);
        });
        assert.eq(0, db.getRole('role1').roles.length);

        assert.throws(function() {
            db.grantRolesToRole("role1", ['role3']);
        });
        assert.eq(0, db.getRole('role1').roles.length);
    })();

    (function testRevokeRolesFromRole() {
        jsTestLog("Testing revokeRolesFromRole");

        // Try to revoke a role that doesn't exist
        // Should not error but should do nothing.
        assert.doesNotThrow(function() {
            db.revokeRolesFromRole("role2", ['fakeRole']);
        });

        // Try to revoke role3 from role2 even though role2 does not contain role3.
        // Should not error but should do nothing.
        assert.doesNotThrow(function() {
            db.revokeRolesFromRole("role2", ['role3']);
        });
        assert.eq(2, db.getRole("role2").roles.length);

        // Must revoke something
        assert.throws(function() {
            db.revokeRolesFromRole("role2", []);
        });

        // Try to remove from built-in role
        assert.throws(function() {
            db.revokeRolesFromRole("readWrite", ['read']);
        });

    })();

    (function testGrantPrivilegesToRole() {
        jsTestLog("Testing grantPrivilegesToRole");

        // Must grant something
        assert.throws(function() {
            db.grantPrivilegesToRole("role1", []);
        });

        var basicPriv = {resource: {db: 'test', collection: ""}, actions: ['find']};

        // Invalid first argument
        assert.throws(function() {
            db.grantPrivilegesToRole(["role1", "role2"], [basicPriv]);
        });

        // Try to grant to role that doesn't exist
        assert.throws(function() {
            db.grantPrivilegesToRole("fakeRole", [basicPriv]);
        });

        // Test with invalid privileges
        var badPrivs = [];
        badPrivs.push("find");
        badPrivs.push({resource: {db: 'test', collection: ""}, actions: ['fakeAction']});
        badPrivs.push({resource: {db: ['test'], collection: ""}, actions: ['find']});
        badPrivs.push({resource: {db: 'test', collection: ""}});
        badPrivs.push({actions: ['find']});
        badPrivs.push({resource: {db: 'test', collection: ""}, actions: []});
        badPrivs.push({resource: {db: 'test'}, actions: ['find']});
        badPrivs.push({resource: {collection: 'test'}, actions: ['find']});
        badPrivs.push({resource: {}, actions: ['find']});
        badPrivs.push({resource: {db: 'test', collection: "", cluster: true}, actions: ['find']});

        for (var i = 0; i < badPrivs.length; i++) {
            assert.throws(function() {
                db.grantPrivilegesToRole("role1", [badPrivs[i]]);
            });
        }

        assert.eq(0, db.getRole('role1', {showPrivileges: true}).privileges.length);
    })();

    (function testRevokePrivilegesFromRole() {
        jsTestLog("Testing revokePrivilegesFromRole");

        // Try to revoke a privilege the role doesn't have
        // Should not error but should do nothing.
        assert.doesNotThrow(function() {
            db.revokePrivilegesFromRole(
                "role3", [{resource: {db: "test", collection: "foobar"}, actions: ["insert"]}]);
        });
        assert.eq(0, db.getRole("role2", {showPrivileges: true}).privileges.length);

        // Must revoke something
        assert.throws(function() {
            db.revokePrivilegesFromRole("role3", []);
        });

        // Try to remove from built-in role
        assert.throws(function() {
            db.revokePrivilegesFromRole(
                "readWrite", [{resource: {db: 'test', collection: ''}, actions: ['find']}]);
        });

        var basicPriv = {resource: {db: 'test', collection: ""}, actions: ['find']};

        // Invalid first argument
        assert.throws(function() {
            db.revokePrivilegesFromRole(["role3", "role2"], [basicPriv]);
        });

        // Try to revoke from role that doesn't exist
        assert.throws(function() {
            db.revokePrivilegesToRole("fakeRole", [basicPriv]);
        });

        // Test with invalid privileges
        var badPrivs = [];
        badPrivs.push("find");
        badPrivs.push({resource: {db: 'test', collection: ""}, actions: ['fakeAction']});
        badPrivs.push({resource: {db: ['test'], collection: ""}, actions: ['find']});
        badPrivs.push({resource: {db: 'test', collection: ""}});
        badPrivs.push({actions: ['find']});
        badPrivs.push({resource: {db: 'test', collection: ""}, actions: []});
        badPrivs.push({resource: {db: 'test'}, actions: ['find']});
        badPrivs.push({resource: {collection: 'test'}, actions: ['find']});
        badPrivs.push({resource: {}, actions: ['find']});
        badPrivs.push({resource: {db: 'test', collection: "", cluster: true}, actions: ['find']});

        for (var i = 0; i < badPrivs.length; i++) {
            assert.throws(function() {
                db.revokePrivilegesFromRole("role3", [badPrivs[i]]);
            });
        }

        assert.eq(1, db.getRole('role3', {showPrivileges: true}).privileges.length);
    })();

    (function testRolesInfo() {
        jsTestLog("Testing rolesInfo");

        // Try to get role that does not exist
        assert.eq(null, db.getRole('fakeRole'));

        // Pass wrong type for role name
        assert.throws(function() {
            db.getRole(5);
        });

        assert.throws(function() {
            db.getRole([]);
        });

        assert.throws(function() {
            db.getRole(['role1', 'role2']);
        });
    })();

    (function testDropRole() {
        jsTestLog("Testing dropRole");

        // Try to drop a role that doesn't exist
        // Should not error but should do nothing
        assert.doesNotThrow(function() {
            db.dropRole('fakeRole');
        });

        // Try to drop a built-in role
        assert.throws(function() {
            db.dropRole('read');
        });

        assert.eq(3, db.getRoles().length);
    })();

    // dropAllRolesFromDatabase ignores its arguments, so there's nothing to test for it.
}

jsTest.log('Test standalone');
var conn = MongoRunner.runMongod({auth: ''});
runTest(conn);
MongoRunner.stopMongod(conn.port);

jsTest.log('Test sharding');
var st = new ShardingTest({shards: 2, config: 3, keyFile: 'jstests/libs/key1'});
runTest(st.s);
st.stop();
