/**
 * This tests that all the different commands for role manipulation all work properly for all valid
 * forms of input.
 */
function runAllRoleManagementCommandsTests(conn, writeConcern) {
    'use strict';

    var hasAuthzError = function(result) {
        assert(result.hasWriteError());
        assert.eq(ErrorCodes.Unauthorized, result.getWriteError().code);
    };

    var userAdminConn = new Mongo(conn.host);
    var testUserAdmin = userAdminConn.getDB('test');
    var adminUserAdmin = userAdminConn.getDB('admin');
    adminUserAdmin.createUser({user: 'userAdmin', pwd: 'pwd', roles: ['userAdminAnyDatabase']},
                              writeConcern);
    adminUserAdmin.auth('userAdmin', 'pwd');
    testUserAdmin.createUser({user: 'testUser', pwd: 'pwd', roles: []}, writeConcern);
    var db = conn.getDB('test');
    assert(db.auth('testUser', 'pwd'));

    // At this point there are 3 databases handles in use. - "testUserAdmin" and "adminUserAdmin"
    // are handles to the "test" and "admin" dbs respectively.  They are on the same connection,
    // which has been auth'd as a user with the 'userAdminAnyDatabase' role.  This will be used
    // for manipulating the user defined roles used in the test. "db" is a handle to the test
    // database on a different connection that has been auth'd as "testUser".  This is the
    // connection that will be used to test that the access control is correct after manipulating
    // the roles assigned to "testUser".

    (function testCreateRole() {
        jsTestLog("Testing createRole");

        testUserAdmin.createRole({role: "testRole1", roles: ['read'], privileges: []},
                                 writeConcern);
        testUserAdmin.createRole({
            role: "testRole2",
            roles: [],
            privileges: [{resource: {db: 'test', collection: 'foo'}, actions: ['insert']}]
        },
                                 writeConcern);
        testUserAdmin.createRole({
            role: "testRole3",
            roles: ['testRole1', {role: 'testRole2', db: 'test'}],
            privileges: []
        },
                                 writeConcern);
        testUserAdmin.createRole({role: "testRole4", roles: [], privileges: []}, writeConcern);
        adminUserAdmin.createRole({
            role: "adminRole",
            roles: [],
            privileges: [{resource: {cluster: true}, actions: ['connPoolSync']}]
        },
                                  writeConcern);

        testUserAdmin.updateUser(
            'testUser', {roles: [{role: 'adminRole', db: 'admin'}]}, writeConcern);
        assert.throws(function() {
            db.foo.findOne();
        });
        hasAuthzError(db.foo.insert({a: 1}));
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.commandWorked(db.adminCommand('connPoolSync'));

        testUserAdmin.updateUser('testUser', {roles: ['testRole1']}, writeConcern);
        assert.doesNotThrow(function() {
            db.foo.findOne();
        });
        assert.eq(0, db.foo.count());
        hasAuthzError(db.foo.insert({a: 1}));
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.commandFailedWithCode(db.adminCommand('connPoolSync'), ErrorCodes.Unauthorized);

        testUserAdmin.updateUser('testUser', {roles: ['testRole2']}, writeConcern);
        assert.throws(function() {
            db.foo.findOne();
        });
        assert.writeOK(db.foo.insert({a: 1}));
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.commandFailedWithCode(db.adminCommand('connPoolSync'), ErrorCodes.Unauthorized);

        testUserAdmin.updateUser('testUser', {roles: ['testRole3']}, writeConcern);
        assert.doesNotThrow(function() {
            db.foo.findOne();
        });
        assert.eq(1, db.foo.count());
        assert.writeOK(db.foo.insert({a: 1}));
        assert.eq(2, db.foo.count());
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(1, db.foo.findOne().a);
        assert.commandFailedWithCode(db.adminCommand('connPoolSync'), ErrorCodes.Unauthorized);

        testUserAdmin.updateUser(
            'testUser', {roles: [{role: 'testRole4', db: 'test'}]}, writeConcern);
        assert.throws(function() {
            db.foo.findOne();
        });
        hasAuthzError(db.foo.insert({a: 1}));
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.commandFailedWithCode(db.adminCommand('connPoolSync'), ErrorCodes.Unauthorized);
    })();

    (function testUpdateRole() {
        jsTestLog("Testing updateRole");

        testUserAdmin.updateRole(
            'testRole4', {roles: [{role: 'testRole2', db: 'test'}, "testRole2"]}, writeConcern);
        assert.throws(function() {
            db.foo.findOne();
        });
        assert.writeOK(db.foo.insert({a: 1}));
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.commandFailedWithCode(db.adminCommand('connPoolSync'), ErrorCodes.Unauthorized);

        testUserAdmin.updateRole(
            'testRole4',
            {privileges: [{resource: {db: 'test', collection: ''}, actions: ['find']}]},
            writeConcern);
        assert.doesNotThrow(function() {
            db.foo.findOne();
        });
        assert.eq(3, db.foo.count());
        assert.writeOK(db.foo.insert({a: 1}));
        assert.eq(4, db.foo.count());
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(1, db.foo.findOne().a);
        assert.commandFailedWithCode(db.adminCommand('connPoolSync'), ErrorCodes.Unauthorized);

        testUserAdmin.updateRole('testRole4', {roles: []}, writeConcern);
        assert.doesNotThrow(function() {
            db.foo.findOne();
        });
        assert.eq(4, db.foo.count());
        hasAuthzError(db.foo.insert({a: 1}));
        assert.eq(4, db.foo.count());
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(1, db.foo.findOne().a);
        assert.commandFailedWithCode(db.adminCommand('connPoolSync'), ErrorCodes.Unauthorized);

        testUserAdmin.updateUser(
            'testUser', {roles: [{role: 'adminRole', db: 'admin'}]}, writeConcern);
        adminUserAdmin.updateRole('adminRole', {roles: [{role: 'read', db: 'test'}]}, writeConcern);
        assert.doesNotThrow(function() {
            db.foo.findOne();
        });
        assert.eq(4, db.foo.count());
        hasAuthzError(db.foo.insert({a: 1}));
        assert.eq(4, db.foo.count());
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(1, db.foo.findOne().a);
        assert.commandWorked(db.adminCommand('connPoolSync'));
    })();

    (function testGrantRolesToRole() {
        jsTestLog("Testing grantRolesToRole");

        assert.commandFailedWithCode(db.adminCommand('serverStatus'), ErrorCodes.Unauthorized);

        adminUserAdmin.grantRolesToRole(
            "adminRole",
            ['clusterMonitor', {role: 'read', db: 'test'}, {role: 'testRole2', db: 'test'}],
            writeConcern);
        assert.doesNotThrow(function() {
            db.foo.findOne();
        });
        assert.eq(4, db.foo.count());
        assert.writeOK(db.foo.insert({a: 1}));
        assert.eq(5, db.foo.count());
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(1, db.foo.findOne().a);
        assert.commandWorked(db.adminCommand('connPoolSync'));
        assert.commandWorked(db.adminCommand('serverStatus'));
    })();

    (function testRevokeRolesFromRole() {
        jsTestLog("Testing revokeRolesFromRole");

        adminUserAdmin.revokeRolesFromRole(
            "adminRole",
            ['clusterMonitor', {role: 'read', db: 'test'}, {role: 'testRole2', db: 'test'}],
            writeConcern);
        assert.throws(function() {
            db.foo.findOne();
        });
        hasAuthzError(db.foo.insert({a: 1}));
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.commandWorked(db.adminCommand('connPoolSync'));
        assert.commandFailedWithCode(db.adminCommand('serverStatus'), ErrorCodes.Unauthorized);
    })();

    (function testGrantPrivilegesToRole() {
        jsTestLog("Testing grantPrivilegesToRole");

        adminUserAdmin.grantPrivilegesToRole(
            'adminRole',
            [
              {resource: {cluster: true}, actions: ['serverStatus']},
              {resource: {db: "", collection: ""}, actions: ['find']}
            ],
            writeConcern);
        assert.doesNotThrow(function() {
            db.foo.findOne();
        });
        hasAuthzError(db.foo.insert({a: 1}));
        assert.eq(5, db.foo.count());
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(1, db.foo.findOne().a);
        assert.commandWorked(db.adminCommand('connPoolSync'));
        assert.commandWorked(db.adminCommand('serverStatus'));

        testUserAdmin.updateUser('testUser', {roles: ['testRole2']}, writeConcern);
        testUserAdmin.grantPrivilegesToRole(
            'testRole2',
            [
              {resource: {db: 'test', collection: ''}, actions: ['insert', 'update']},
              {resource: {db: 'test', collection: 'foo'}, actions: ['find']}
            ],
            writeConcern);
        assert.doesNotThrow(function() {
            db.foo.findOne();
        });
        assert.writeOK(db.foo.insert({a: 1}));
        assert.eq(6, db.foo.count());
        assert.writeOK(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(2, db.foo.findOne().a);
        assert.commandFailedWithCode(db.adminCommand('connPoolSync'), ErrorCodes.Unauthorized);
        assert.commandFailedWithCode(db.adminCommand('serverStatus'), ErrorCodes.Unauthorized);
    })();

    (function testRevokePrivilegesFromRole() {
        jsTestLog("Testing revokePrivilegesFromRole");

        testUserAdmin.revokePrivilegesFromRole(
            'testRole2',
            [{resource: {db: 'test', collection: ''}, actions: ['insert', 'update', 'find']}],
            writeConcern);
        assert.doesNotThrow(function() {
            db.foo.findOne();
        });
        assert.writeOK(db.foo.insert({a: 1}));
        assert.eq(7, db.foo.count());
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}, false, true));
        assert.eq(2, db.foo.findOne().a);
        assert.commandFailedWithCode(db.adminCommand('connPoolSync'), ErrorCodes.Unauthorized);
        assert.commandFailedWithCode(db.adminCommand('serverStatus'), ErrorCodes.Unauthorized);
    })();

    (function testRolesInfo() {
        jsTestLog("Testing rolesInfo");

        var res = testUserAdmin.runCommand({rolesInfo: 'testRole1'});
        assert.eq(1, res.roles.length);
        assert.eq("testRole1", res.roles[0].role);
        assert.eq("test", res.roles[0].db);
        assert.eq(1, res.roles[0].roles.length);
        assert.eq("read", res.roles[0].roles[0].role);

        res = testUserAdmin.runCommand({rolesInfo: {role: 'testRole1', db: 'test'}});
        assert.eq(1, res.roles.length);
        assert.eq("testRole1", res.roles[0].role);
        assert.eq("test", res.roles[0].db);
        assert.eq(1, res.roles[0].roles.length);
        assert.eq("read", res.roles[0].roles[0].role);

        res =
            testUserAdmin.runCommand({rolesInfo: ['testRole1', {role: 'adminRole', db: 'admin'}]});
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

        res = testUserAdmin.runCommand({rolesInfo: "testRole1", showPrivileges: 'asUserFragment'});
        assert(res.userFragment);
        assert.eq(1, res.userFragment.roles.length);
        assert.eq([{role: "testRole1", db: "test"}], res.userFragment.roles);
        assert.eq(2, res.userFragment.inheritedRoles.length);
        assert.contains({role: "testRole1", db: "test"}, res.userFragment.inheritedRoles);
        assert.contains({role: "read", db: "test"}, res.userFragment.inheritedRoles);
        assert.gt(res.userFragment.inheritedPrivileges.length, 0);

        res = testUserAdmin.runCommand(
            {rolesInfo: ['testRole1', 'testRole2'], showPrivileges: 'asUserFragment'});
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

        testUserAdmin.grantRolesToUser('testUser', ['testRole4'], writeConcern);

        assert.doesNotThrow(function() {
            db.foo.findOne();
        });
        assert.writeOK(db.foo.insert({a: 1}));
        assert.eq(8, db.foo.count());

        testUserAdmin.dropRole('testRole2', writeConcern);

        assert.doesNotThrow(function() {
            db.foo.findOne();
        });
        hasAuthzError(db.foo.insert({a: 1}));
        assert.eq(8, db.foo.count());

        assert.eq(3, testUserAdmin.getRoles().length);
    })();

    (function testDropAllRolesFromDatabase() {
        jsTestLog("Testing dropAllRolesFromDatabase");

        assert.doesNotThrow(function() {
            db.foo.findOne();
        });
        assert.eq(3, testUserAdmin.getRoles().length);

        testUserAdmin.dropAllRoles(writeConcern);

        assert.throws(function() {
            db.foo.findOne();
        });
        assert.eq(0, testUserAdmin.getRoles().length);
    })();
}
