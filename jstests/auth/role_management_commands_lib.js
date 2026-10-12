/**
 * This tests that all the different commands for role manipulation all work properly for all valid
 * forms of input.
 */
export function runAllRoleManagementCommandsTests(conn, writeConcern) {
    let hasAuthzError = function (result) {
        assert(result instanceof WriteCommandError);
        assert.eq(ErrorCodes.Unauthorized, result.code);
    };

    let userAdminConn = new Mongo(conn.host);
    let testUserAdmin = userAdminConn.getDB("test");
    let adminUserAdmin = userAdminConn.getDB("admin");
    adminUserAdmin.createUser({user: "userAdmin", pwd: "pwd", roles: ["userAdminAnyDatabase"]}, writeConcern);
    adminUserAdmin.auth("userAdmin", "pwd");
    testUserAdmin.createUser({user: "testUser", pwd: "pwd", roles: []}, writeConcern);
    let db = conn.getDB("test");
    assert(db.auth("testUser", "pwd"));

    // At this point there are 3 databases handles in use. - "testUserAdmin" and "adminUserAdmin"
    // are handles to the "test" and "admin" dbs respectively.  They are on the same connection,
    // which has been auth'd as a user with the 'userAdminAnyDatabase' role.  This will be used
    // for manipulating the user defined roles used in the test. "db" is a handle to the test
    // database on a different connection that has been auth'd as "testUser".  This is the
    // connection that will be used to test that the access control is correct after manipulating
    // the roles assigned to "testUser".

    (function testCreateRole() {
        jsTestLog("Testing createRole");

        testUserAdmin.createRole({role: "testRole1", roles: ["read"], privileges: []}, writeConcern);
        testUserAdmin.createRole(
            {
                role: "testRole2",
                roles: [],
                privileges: [{resource: {db: "test", collection: "foo"}, actions: ["insert"]}],
            },
            writeConcern,
        );
        testUserAdmin.createRole(
            {
                role: "testRole3",
                roles: ["testRole1", {role: "testRole2", db: "test"}],
                privileges: [],
            },
            writeConcern,
        );
        testUserAdmin.createRole({role: "testRole4", roles: [], privileges: []}, writeConcern);
        adminUserAdmin.createRole(
            {
                role: "adminRole",
                roles: [],
                privileges: [{resource: {cluster: true}, actions: ["connPoolSync"]}],
            },
            writeConcern,
        );

        testUserAdmin.updateUser("testUser", {roles: [{role: "adminRole", db: "admin"}]}, writeConcern);
        assert.throws(function () {
            db.foo.findOne();
        });
        hasAuthzError(db.foo.insert({a: 1}));
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.commandWorked(db.adminCommand("connPoolSync"));

        testUserAdmin.updateUser("testUser", {roles: ["testRole1"]}, writeConcern);
        assert.doesNotThrow(function () {
            db.foo.findOne();
        });
        assert.eq(0, db.foo.count());
        hasAuthzError(db.foo.insert({a: 1}));
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.commandFailedWithCode(db.adminCommand("connPoolSync"), ErrorCodes.Unauthorized);

        testUserAdmin.updateUser("testUser", {roles: ["testRole2"]}, writeConcern);
        assert.throws(function () {
            db.foo.findOne();
        });
        assert.commandWorked(db.foo.insert({a: 1}));
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.commandFailedWithCode(db.adminCommand("connPoolSync"), ErrorCodes.Unauthorized);

        testUserAdmin.updateUser("testUser", {roles: ["testRole3"]}, writeConcern);
        assert.doesNotThrow(function () {
            db.foo.findOne();
        });
        assert.eq(1, db.foo.count());
        assert.commandWorked(db.foo.insert({a: 1}));
        assert.eq(2, db.foo.count());
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(1, db.foo.findOne().a);
        assert.commandFailedWithCode(db.adminCommand("connPoolSync"), ErrorCodes.Unauthorized);

        testUserAdmin.updateUser("testUser", {roles: [{role: "testRole4", db: "test"}]}, writeConcern);
        assert.throws(function () {
            db.foo.findOne();
        });
        hasAuthzError(db.foo.insert({a: 1}));
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.commandFailedWithCode(db.adminCommand("connPoolSync"), ErrorCodes.Unauthorized);
    })();

    (function testUpdateRole() {
        jsTestLog("Testing updateRole");

        testUserAdmin.updateRole("testRole4", {roles: [{role: "testRole2", db: "test"}, "testRole2"]}, writeConcern);
        assert.throws(function () {
            db.foo.findOne();
        });
        assert.commandWorked(db.foo.insert({a: 1}));
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.commandFailedWithCode(db.adminCommand("connPoolSync"), ErrorCodes.Unauthorized);

        testUserAdmin.updateRole(
            "testRole4",
            {
                privileges: [{resource: {db: "test", collection: ""}, actions: ["find"]}],
            },
            writeConcern,
        );
        assert.doesNotThrow(function () {
            db.foo.findOne();
        });
        assert.eq(3, db.foo.count());
        assert.commandWorked(db.foo.insert({a: 1}));
        assert.eq(4, db.foo.count());
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(1, db.foo.findOne().a);
        assert.commandFailedWithCode(db.adminCommand("connPoolSync"), ErrorCodes.Unauthorized);

        testUserAdmin.updateRole("testRole4", {roles: []}, writeConcern);
        assert.doesNotThrow(function () {
            db.foo.findOne();
        });
        assert.eq(4, db.foo.count());
        hasAuthzError(db.foo.insert({a: 1}));
        assert.eq(4, db.foo.count());
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(1, db.foo.findOne().a);
        assert.commandFailedWithCode(db.adminCommand("connPoolSync"), ErrorCodes.Unauthorized);

        testUserAdmin.updateUser("testUser", {roles: [{role: "adminRole", db: "admin"}]}, writeConcern);
        adminUserAdmin.updateRole("adminRole", {roles: [{role: "read", db: "test"}]}, writeConcern);
        assert.doesNotThrow(function () {
            db.foo.findOne();
        });
        assert.eq(4, db.foo.count());
        hasAuthzError(db.foo.insert({a: 1}));
        assert.eq(4, db.foo.count());
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(1, db.foo.findOne().a);
        assert.commandWorked(db.adminCommand("connPoolSync"));
    })();

    (function testGrantRolesToRole() {
        jsTestLog("Testing grantRolesToRole");

        assert.commandFailedWithCode(db.adminCommand("serverStatus"), ErrorCodes.Unauthorized);

        adminUserAdmin.grantRolesToRole(
            "adminRole",
            ["clusterMonitor", {role: "read", db: "test"}, {role: "testRole2", db: "test"}],
            writeConcern,
        );
        assert.doesNotThrow(function () {
            db.foo.findOne();
        });
        assert.eq(4, db.foo.count());
        assert.commandWorked(db.foo.insert({a: 1}));
        assert.eq(5, db.foo.count());
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(1, db.foo.findOne().a);
        assert.commandWorked(db.adminCommand("connPoolSync"));
        assert.commandWorked(db.adminCommand("serverStatus"));
    })();

    (function testRevokeRolesFromRole() {
        jsTestLog("Testing revokeRolesFromRole");

        adminUserAdmin.revokeRolesFromRole(
            "adminRole",
            ["clusterMonitor", {role: "read", db: "test"}, {role: "testRole2", db: "test"}],
            writeConcern,
        );
        assert.throws(function () {
            db.foo.findOne();
        });
        hasAuthzError(db.foo.insert({a: 1}));
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.commandWorked(db.adminCommand("connPoolSync"));
        assert.commandFailedWithCode(db.adminCommand("serverStatus"), ErrorCodes.Unauthorized);
    })();

    (function testGrantPrivilegesToRole() {
        jsTestLog("Testing grantPrivilegesToRole");

        adminUserAdmin.grantPrivilegesToRole(
            "adminRole",
            [
                {resource: {cluster: true}, actions: ["serverStatus"]},
                {resource: {db: "", collection: ""}, actions: ["find"]},
            ],
            writeConcern,
        );
        assert.doesNotThrow(function () {
            db.foo.findOne();
        });
        hasAuthzError(db.foo.insert({a: 1}));
        assert.eq(5, db.foo.count());
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(1, db.foo.findOne().a);
        assert.commandWorked(db.adminCommand("connPoolSync"));
        assert.commandWorked(db.adminCommand("serverStatus"));

        testUserAdmin.updateUser("testUser", {roles: ["testRole2"]}, writeConcern);
        testUserAdmin.grantPrivilegesToRole(
            "testRole2",
            [
                {
                    resource: {db: "test", collection: ""},
                    actions: ["insert", "update"],
                },
                {resource: {db: "test", collection: "foo"}, actions: ["find"]},
            ],
            writeConcern,
        );
        assert.doesNotThrow(function () {
            db.foo.findOne();
        });
        assert.commandWorked(db.foo.insert({a: 1}));
        assert.eq(6, db.foo.count());
        assert.commandWorked(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(2, db.foo.findOne().a);
        assert.commandFailedWithCode(db.adminCommand("connPoolSync"), ErrorCodes.Unauthorized);
        assert.commandFailedWithCode(db.adminCommand("serverStatus"), ErrorCodes.Unauthorized);
    })();

    (function testRevokePrivilegesFromRole() {
        jsTestLog("Testing revokePrivilegesFromRole");

        testUserAdmin.revokePrivilegesFromRole(
            "testRole2",
            [
                {
                    resource: {db: "test", collection: ""},
                    actions: ["insert", "update", "find"],
                },
            ],
            writeConcern,
        );
        assert.doesNotThrow(function () {
            db.foo.findOne();
        });
        assert.commandWorked(db.foo.insert({a: 1}));
        assert.eq(7, db.foo.count());
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(2, db.foo.findOne().a);
        assert.commandFailedWithCode(db.adminCommand("connPoolSync"), ErrorCodes.Unauthorized);
        assert.commandFailedWithCode(db.adminCommand("serverStatus"), ErrorCodes.Unauthorized);
    })();

    (function testRolesInfo() {
        jsTestLog("Testing rolesInfo");

        let res = testUserAdmin.runCommand({rolesInfo: "testRole1"});
        assert.eq(1, res.roles.length);
        assert.eq("testRole1", res.roles[0].role);
        assert.eq("test", res.roles[0].db);
        assert.eq(1, res.roles[0].roles.length);
        assert.eq("read", res.roles[0].roles[0].role);

        res = testUserAdmin.runCommand({
            rolesInfo: {role: "testRole1", db: "test"},
        });
        assert.eq(1, res.roles.length);
        assert.eq("testRole1", res.roles[0].role);
        assert.eq("test", res.roles[0].db);
        assert.eq(1, res.roles[0].roles.length);
        assert.eq("read", res.roles[0].roles[0].role);

        res = testUserAdmin.runCommand({rolesInfo: ["testRole1", {role: "adminRole", db: "admin"}]});
        assert.eq(2, res.roles.length);
        assert.eq("testRole1", res.roles[0].role);
        assert.eq("test", res.roles[0].db);
        assert.eq(1, res.roles[0].roles.length);
        assert.eq("read", res.roles[0].roles[0].role);
        assert.eq("adminRole", res.roles[1].role);
        assert.eq("admin", res.roles[1].db);
        assert.eq(0, res.roles[1].roles.length);

        res = testUserAdmin.runCommand({rolesInfo: 1});
        assert.eq(4, res.roles.length);

        res = testUserAdmin.runCommand({rolesInfo: 1, showBuiltinRoles: 1});
        assert.eq(10, res.roles.length);

        res = testUserAdmin.runCommand({
            rolesInfo: "testRole1",
            showPrivileges: "asUserFragment",
        });
        assert(res.userFragment);
        assert.eq(1, res.userFragment.roles.length);
        assert.eq([{role: "testRole1", db: "test"}], res.userFragment.roles);
        assert.eq(2, res.userFragment.inheritedRoles.length);
        assert.contains({role: "testRole1", db: "test"}, res.userFragment.inheritedRoles);
        assert.contains({role: "read", db: "test"}, res.userFragment.inheritedRoles);
        assert.gt(res.userFragment.inheritedPrivileges.length, 0);

        res = testUserAdmin.runCommand({rolesInfo: ["testRole1", "testRole2"], showPrivileges: "asUserFragment"});
        assert(res.userFragment);
        assert.eq(2, res.userFragment.roles.length);
        assert.contains({role: "testRole1", db: "test"}, res.userFragment.roles);
        assert.contains({role: "testRole2", db: "test"}, res.userFragment.roles);
        assert.eq(3, res.userFragment.inheritedRoles.length);
        assert.contains({role: "testRole1", db: "test"}, res.userFragment.inheritedRoles);
        assert.contains({role: "testRole2", db: "test"}, res.userFragment.inheritedRoles);
        assert.contains({role: "read", db: "test"}, res.userFragment.inheritedRoles);
        assert.gt(res.userFragment.inheritedPrivileges.length, 0);
    })();

    (function testDropRole() {
        jsTestLog("Testing dropRole");

        testUserAdmin.grantRolesToUser("testUser", ["testRole4"], writeConcern);

        assert.doesNotThrow(function () {
            db.foo.findOne();
        });
        assert.commandWorked(db.foo.insert({a: 1}));
        assert.eq(8, db.foo.count());

        assert.gt(
            adminUserAdmin.system.users.countDocuments({"roles.role": "testRole2"}),
            0,
            "testRole2 should be assigned to users before calling dropRole in order to test that references are cleaned up correctly",
        );
        assert.gt(
            adminUserAdmin.system.roles.countDocuments({"roles.role": "testRole2"}),
            0,
            "testRole2 should be referenced by at least one other role before calling dropRole in order to test that references are cleaned up correctly",
        );
        assert.eq(1, adminUserAdmin.system.users.countDocuments({"roles.role": "testRole4"}));
        assert.eq(1, adminUserAdmin.system.roles.countDocuments({"roles.role": "testRole1"}));

        testUserAdmin.dropRole("testRole2", writeConcern);

        assert.doesNotThrow(function () {
            db.foo.findOne();
        });
        hasAuthzError(db.foo.insert({a: 1}));
        assert.eq(8, db.foo.count());

        assert.eq(3, testUserAdmin.getRoles().length);
        assert.eq(
            adminUserAdmin.system.users.countDocuments({"roles.role": "testRole2"}),
            0,
            "dropRole should clean up all role references in system.users",
        );
        assert.eq(
            adminUserAdmin.system.roles.countDocuments({"roles.role": "testRole2"}),
            0,
            "dropRole should clean up all role references in system.roles",
        );
        assert.eq(
            1,
            adminUserAdmin.system.users.countDocuments({"roles.role": "testRole4"}),
            "dropRole should not remove references to other roles in system.users",
        );
        assert.eq(
            1,
            adminUserAdmin.system.roles.countDocuments({"roles.role": "testRole1"}),
            "dropRole should not remove references to other roles in system.roles",
        );
    })();

    (function testDropAllRolesFromDatabase() {
        jsTestLog("Testing dropAllRolesFromDatabase");

        assert.doesNotThrow(function () {
            db.foo.findOne();
        });
        assert.eq(3, testUserAdmin.getRoles().length);
        assert.gt(
            adminUserAdmin.system.users.countDocuments({"roles.db": "test"}),
            0,
            "Some roles should be assigned to users before calling dropAllRoles in order to test that references are cleaned up correctly",
        );

        // Create a role with db = "admin" so it will not get dropped by db.dropAllRoles().
        // The role contains a reference to a role on the "test" db which should be cleaned up by db.dropAllRoles().
        adminUserAdmin.createRole(
            {
                role: "testNestedRoleReference",
                roles: [
                    {role: "testRole1", db: "test"},
                    {role: "adminRole", db: "admin"},
                ],
                privileges: [{resource: {cluster: true}, actions: ["connPoolSync"]}],
            },
            writeConcern,
        );
        adminUserAdmin.createUser(
            {
                user: "testMixedRolesUser",
                pwd: "pwd",
                roles: [
                    {role: "testRole1", db: "test"},
                    {role: "adminRole", db: "admin"},
                ],
            },
            writeConcern,
        );
        assert.eq(1, adminUserAdmin.system.roles.countDocuments({"roles.role": "adminRole"}));
        assert.eq(1, adminUserAdmin.system.users.countDocuments({"roles.role": "adminRole"}));
        assert.eq(1, adminUserAdmin.system.users.countDocuments({"roles.role": "userAdminAnyDatabase"}));
        assert.gt(
            adminUserAdmin.system.roles.countDocuments({
                "roles.db": "test",
                db: {$ne: "test"},
            }),
            0,
            'Some "test" roles should be referenced by other non-"test" roles before calling dropAllRoles in order to verify that references are cleaned up correctly',
        );

        testUserAdmin.dropAllRoles(writeConcern);

        assert.throws(function () {
            db.foo.findOne();
        });
        assert.eq(0, testUserAdmin.getRoles().length);
        assert.eq(
            adminUserAdmin.system.roles.countDocuments({
                "roles.db": "test",
                db: {$ne: "test"},
            }),
            0,
            "dropAllRoles should clean up all role references in system.roles",
        );
        assert.eq(
            adminUserAdmin.system.users.countDocuments({"roles.db": "test"}),
            0,
            "dropAllRoles should clean up all role references in system.users",
        );
        assert.eq(
            1,
            adminUserAdmin.system.roles.countDocuments({"roles.role": "adminRole"}),
            "dropAllRoles should not remove references to roles on other dbs in system.roles",
        );
        assert.eq(
            1,
            adminUserAdmin.system.users.countDocuments({"roles.role": "adminRole"}),
            "dropAllRoles should not remove references to roles on other dbs in system.users",
        );
        assert.eq(
            1,
            adminUserAdmin.system.users.countDocuments({"roles.role": "userAdminAnyDatabase"}),
            "dropAllRoles should not modify users that do not reference its roles",
        );
    })();
}
