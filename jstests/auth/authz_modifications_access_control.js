/**
 * This tests that the proper access control is enforced around modifications to user and role data.
 */

function runTest(conn) {
    var authzErrorCode = 13;

    conn.getDB('admin').createUser(
        {user: 'userAdmin', pwd: 'pwd', roles: ['userAdminAnyDatabase']});

    var userAdminConn = new Mongo(conn.host);
    userAdminConn.getDB('admin').auth('userAdmin', 'pwd');
    var testUserAdmin = userAdminConn.getDB('test');
    var adminUserAdmin = userAdminConn.getDB('admin');
    testUserAdmin.createRole({role: 'testRole', roles: [], privileges: []});
    adminUserAdmin.createRole({role: 'adminRole', roles: [], privileges: []});
    testUserAdmin.createUser(
        {user: 'spencer', pwd: 'pwd', roles: ['testRole', {role: 'adminRole', db: 'admin'}]});
    adminUserAdmin.createUser({user: 'otherUser', pwd: 'pwd', roles: []});

    var db = conn.getDB('test');
    db.auth('spencer', 'pwd');
    var admindb = conn.getDB('admin');

    // "adminUserAdmin" and "testUserAdmin" are handles to the "admin" and "test" dbs, respectively.
    // Both are on the same connection, which has been auth'd as a user with 'userAdminAnyDatabase'.
    // "db" and "admindb" are also handles to the "test" and "admin" dbs, but on a connection that
    // has been auth'd as the user spencer@test.  "testUserAdmin" and "adminUserAdmin" will be used
    // to control what privileges spencer@test has (and thus are usable by "db" and "admindb"),
    // while "db" and "admindb" will be used for the actual permission checks that are being tested.

    (function testCreateUser() {
        jsTestLog("Testing user creation");

        var res = db.runCommand({createUser: 'andy', pwd: 'pwd', roles: []});
        assert.commandFailedWithCode(res, authzErrorCode);

        testUserAdmin.grantPrivilegesToRole(
            'testRole', [{resource: {db: 'test', collection: ''}, actions: ['createUser']}]);

        assert.commandWorked(db.runCommand({createUser: 'andy', pwd: 'pwd', roles: []}));

        res = admindb.runCommand({createUser: 'andy', pwd: 'pwd', roles: []});
        assert.commandFailedWithCode(res, authzErrorCode);
    })();

    (function testCreateRole() {
        jsTestLog("Testing role creation");

        var res = db.runCommand({createRole: 'testRole2', roles: [], privileges: []});
        assert.commandFailedWithCode(res, authzErrorCode);

        testUserAdmin.grantPrivilegesToRole(
            'testRole', [{resource: {db: 'test', collection: ''}, actions: ['createRole']}]);

        assert.commandWorked(db.runCommand({createRole: 'testRole2', roles: [], privileges: []}));

        res = admindb.runCommand({createRole: 'testRole2', roles: [], privileges: []});
        assert.commandFailedWithCode(res, authzErrorCode);
    })();

    (function() {
        jsTestLog("Testing role creation, of user-defined roles with same name as built-in roles");

        var cmdObj = {createRole: "readWrite", roles: [], privileges: []};
        var res = adminUserAdmin.runCommand(cmdObj);
        assert.commandFailed(res, tojson(cmdObj));

        var roleObj = adminUserAdmin.system.roles.findOne({role: "readWrite", db: "admin"});
        // double check that no role object named "readWrite" has been created
        assert(!roleObj, "user-defined \"readWrite\" role was created: " + tojson(roleObj));

    })();

    (function testViewUser() {
        jsTestLog("Testing viewing user information");

        var res = db.runCommand({usersInfo: 'andy'});
        assert.commandFailedWithCode(res, authzErrorCode);

        testUserAdmin.grantPrivilegesToRole(
            'testRole', [{resource: {db: 'test', collection: ''}, actions: ['viewUser']}]);

        assert.commandWorked(db.runCommand({usersInfo: 'andy'}));

        res = admindb.runCommand({usersInfo: 'andy'});
        assert.commandFailedWithCode(res, authzErrorCode);
    })();

    (function testViewRole() {
        jsTestLog("Testing viewing role information");

        var res = db.runCommand({rolesInfo: 'testRole2'});
        assert.commandFailedWithCode(res, authzErrorCode);

        testUserAdmin.grantPrivilegesToRole(
            'testRole', [{resource: {db: 'test', collection: ''}, actions: ['viewRole']}]);

        assert.commandWorked(db.runCommand({rolesInfo: 'testRole2'}));

        res = admindb.runCommand({rolesInfo: 'testRole2'});
        assert.commandFailedWithCode(res, authzErrorCode);
    })();

    (function testDropUser() {
        jsTestLog("Testing dropping user");

        var res = db.runCommand({dropUser: 'andy'});
        assert.commandFailedWithCode(res, authzErrorCode);

        testUserAdmin.grantPrivilegesToRole(
            'testRole', [{resource: {db: 'test', collection: ''}, actions: ['dropUser']}]);

        assert.commandWorked(db.runCommand({dropUser: 'andy'}));

        res = admindb.runCommand({dropUser: 'andy'});
        assert.commandFailedWithCode(res, authzErrorCode);
    })();

    (function testDropRole() {
        jsTestLog("Testing dropping role");

        var res = db.runCommand({dropRole: 'testRole2'});
        assert.commandFailedWithCode(res, authzErrorCode);

        testUserAdmin.grantPrivilegesToRole(
            'testRole', [{resource: {db: 'test', collection: ''}, actions: ['dropRole']}]);

        assert.commandWorked(db.runCommand({dropRole: 'testRole2'}));

        res = admindb.runCommand({dropRole: 'testRole2'});
        assert.commandFailedWithCode(res, authzErrorCode);
    })();

    (function testGrantRole() {
        jsTestLog("Testing granting roles");

        var res = db.runCommand({createUser: 'andy', pwd: 'pwd', roles: ['read']});
        assert.commandFailedWithCode(res, authzErrorCode);

        res = db.runCommand({grantRolesToUser: 'spencer', roles: ['read']});
        assert.commandFailedWithCode(res, authzErrorCode);

        res = db.runCommand({grantRolesToRole: 'testRole', roles: ['read']});
        assert.commandFailedWithCode(res, authzErrorCode);

        res = admindb.runCommand(
            {grantRolesToUser: 'otherUser', roles: [{role: 'read', db: 'test'}]});
        assert.commandFailedWithCode(res, authzErrorCode);

        testUserAdmin.grantPrivilegesToRole(
            'testRole', [{resource: {db: 'test', collection: ''}, actions: ['grantRole']}]);

        assert.commandWorked(db.runCommand({createUser: 'andy', pwd: 'pwd', roles: ['read']}));
        assert.commandWorked(db.runCommand({grantRolesToUser: 'spencer', roles: ['read']}));
        assert.commandWorked(db.runCommand({grantRolesToRole: 'testRole', roles: ['read']}));

        // Granting roles from other dbs should fail
        res = db.runCommand({grantRolesToUser: 'spencer', roles: [{role: 'read', db: 'other'}]});
        assert.commandFailedWithCode(res, authzErrorCode);

        // Granting roles from this db to users in another db, however, should work
        res = admindb.runCommand(
            {grantRolesToUser: 'otherUser', roles: [{role: 'read', db: 'test'}]});
        assert.commandWorked(res);
    })();

    (function testRevokeRole() {
        jsTestLog("Testing revoking roles");

        var res = db.runCommand({revokeRolesFromUser: 'spencer', roles: ['read']});
        assert.commandFailedWithCode(res, authzErrorCode);

        res = db.runCommand({revokeRolesFromRole: 'testRole', roles: ['read']});
        assert.commandFailedWithCode(res, authzErrorCode);

        res = admindb.runCommand(
            {revokeRolesFromUser: 'otherUser', roles: [{role: 'read', db: 'test'}]});
        assert.commandFailedWithCode(res, authzErrorCode);

        testUserAdmin.grantPrivilegesToRole(
            'testRole', [{resource: {db: 'test', collection: ''}, actions: ['revokeRole']}]);

        assert.commandWorked(db.runCommand({revokeRolesFromUser: 'spencer', roles: ['read']}));
        assert.commandWorked(db.runCommand({revokeRolesFromRole: 'testRole', roles: ['read']}));

        // Revoking roles from other dbs should fail
        res = db.runCommand({revokeRolesFromUser: 'spencer', roles: [{role: 'read', db: 'other'}]});
        assert.commandFailedWithCode(res, authzErrorCode);

        // Revoking roles from this db from users in another db, however, should work
        res = admindb.runCommand(
            {revokeRolesFromUser: 'otherUser', roles: [{role: 'read', db: 'test'}]});
        assert.commandWorked(res);
    })();

    (function testGrantPrivileges() {
        jsTestLog("Testing granting privileges");

        testUserAdmin.revokePrivilegesFromRole(
            'testRole', [{resource: {db: 'test', collection: ''}, actions: ['grantRole']}]);

        var res = db.runCommand({
            createRole: 'testRole2',
            roles: [],
            privileges: [{resource: {db: 'test', collection: ''}, actions: ['find']}]
        });
        assert.commandFailedWithCode(res, authzErrorCode);

        res = db.runCommand({
            grantPrivilegesToRole: 'testRole',
            privileges: [{resource: {db: 'test', collection: ''}, actions: ['find']}]
        });
        assert.commandFailedWithCode(res, authzErrorCode);

        res = admindb.runCommand({
            grantPrivilegesToRole: 'adminRole',
            privileges: [{resource: {db: 'test', collection: ''}, actions: ['find']}]
        });
        assert.commandFailedWithCode(res, authzErrorCode);

        testUserAdmin.grantPrivilegesToRole(
            'testRole', [{resource: {db: 'test', collection: ''}, actions: ['grantRole']}]);

        res = db.runCommand({
            createRole: 'testRole2',
            roles: [],
            privileges: [{resource: {db: 'test', collection: ''}, actions: ['find']}]
        });
        assert.commandWorked(res);

        res = db.runCommand({
            grantPrivilegesToRole: 'testRole',
            privileges: [{resource: {db: 'test', collection: ''}, actions: ['find']}]
        });
        assert.commandWorked(res);

        // Granting privileges from other dbs should fail
        res = db.runCommand({
            grantPrivilegesToRole: 'testRole',
            privileges: [{resource: {db: 'other', collection: ''}, actions: ['find']}]
        });
        assert.commandFailedWithCode(res, authzErrorCode);

        // Granting privileges from this db to users in another db, however, should work
        res = admindb.runCommand({
            grantPrivilegesToRole: 'adminRole',
            privileges: [{resource: {db: 'test', collection: ''}, actions: ['find']}]
        });
        assert.commandWorked(res);
    })();

    (function testRevokePrivileges() {
        jsTestLog("Testing revoking privileges");

        testUserAdmin.revokePrivilegesFromRole(
            'testRole', [{resource: {db: 'test', collection: ''}, actions: ['revokeRole']}]);

        var res = db.runCommand({
            revokePrivilegesFromRole: 'testRole',
            privileges: [{resource: {db: 'test', collection: ''}, actions: ['find']}]
        });
        assert.commandFailedWithCode(res, authzErrorCode);

        res = admindb.runCommand({
            revokePrivilegesFromRole: 'adminRole',
            privileges: [{resource: {db: 'test', collection: ''}, actions: ['find']}]
        });
        assert.commandFailedWithCode(res, authzErrorCode);

        testUserAdmin.grantPrivilegesToRole(
            'testRole', [{resource: {db: 'test', collection: ''}, actions: ['revokeRole']}]);

        res = db.runCommand({
            revokePrivilegesFromRole: 'testRole',
            privileges: [{resource: {db: 'test', collection: ''}, actions: ['find']}]
        });
        assert.commandWorked(res);

        // Revoking privileges from other dbs should fail
        res = db.runCommand({
            revokePrivilegesFromRole: 'testRole',
            privileges: [{resource: {db: 'other', collection: ''}, actions: ['find']}]
        });
        assert.commandFailedWithCode(res, authzErrorCode);

        // Granting privileges from this db to users in another db, however, should work
        res = admindb.runCommand({
            revokePrivilegesFromRole: 'adminRole',
            privileges: [{resource: {db: 'test', collection: ''}, actions: ['find']}]
        });
        assert.commandWorked(res);
    })();
}

jsTest.log('Test standalone');
var conn = MongoRunner.runMongod({auth: ''});
runTest(conn);
MongoRunner.stopMongod(conn.port);

jsTest.log('Test sharding');
var st = new ShardingTest({shards: 2, config: 3, keyFile: 'jstests/libs/key1'});
runTest(st.s);
st.stop();
