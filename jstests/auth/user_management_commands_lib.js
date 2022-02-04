/**
 * This tests that all the different commands for user manipulation all work properly for all valid
 * forms of input.
 */
function runAllUserManagementCommandsTests(conn, writeConcern) {
    'use strict';

    function hasAuthzError(result) {
        assert(result instanceof WriteCommandError);
        assert.eq(ErrorCodes.Unauthorized, result.code);
    }

    const admin = conn.getDB('admin');

    admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']}, writeConcern);
    assert(admin.auth('admin', 'pwd'));
    admin.createUser({
        user: 'userAdmin',
        pwd: 'pwd',
        roles: ['userAdminAnyDatabase'],
        customData: {userAdmin: true}
    },
                     writeConcern);
    admin.logout();

    const userAdminConn = new Mongo(conn.host);
    const userAdmin = userAdminConn.getDB('admin');
    assert(userAdmin.auth('userAdmin', 'pwd'));
    const testUserAdmin = userAdminConn.getDB('test');
    testUserAdmin.createRole({
        role: 'testRole',
        roles: [],
        privileges: [{resource: {db: 'test', collection: ''}, actions: ['viewRole']}],
    },
                             writeConcern);
    userAdmin.createRole({
        role: 'adminRole',
        roles: [],
        privileges: [{resource: {cluster: true}, actions: ['connPoolSync']}]
    },
                         writeConcern);

    const db = conn.getDB('test');

    // At this point there are 2 handles to the "test" database in use - "testUserAdmin" and "db".
    // "testUserAdmin" is on a connection which has been auth'd as a user with the
    // 'userAdminAnyDatabase' role.  This will be used for manipulating the user defined roles
    // used in the test.  "db" is a handle to the test database on a connection that has not
    // yet been authenticated as anyone.  This is the connection that will be used to log in as
    // various users and test that their access control is correct.

    (function testCreateUser() {
        jsTestLog("Testing createUser");

        testUserAdmin.createUser({
            user: "spencer",
            pwd: "pwd",
            customData: {zipCode: 10028},
            roles: ['readWrite', 'testRole', {role: 'adminRole', db: 'admin'}]
        },
                                 writeConcern);
        testUserAdmin.createUser({user: "andy", pwd: "pwd", roles: []}, writeConcern);

        const user = testUserAdmin.getUser('spencer');
        assert.eq(10028, user.customData.zipCode);
        assert(db.auth('spencer', 'pwd'));
        assert.commandWorked(db.foo.insert({a: 1}));
        assert.eq(1, db.foo.findOne().a);
        assert.doesNotThrow(function() {
            db.getRole('testRole');
        });
        assert.commandWorked(db.adminCommand('connPoolSync'));

        db.logout();

        assert(db.auth('andy', 'pwd'));
        hasAuthzError(db.foo.insert({a: 1}));
        assert.throws(function() {
            db.foo.findOne();
        });
        assert.throws(function() {
            db.getRole('testRole');
        });
        db.logout();
    })();

    (function testUpdateUser() {
        jsTestLog("Testing updateUser");

        testUserAdmin.updateUser('spencer', {pwd: 'password', customData: {}}, writeConcern);
        const user1 = testUserAdmin.getUser('spencer');
        assert.eq(null, user1.customData.zipCode);
        assert(!db.auth('spencer', 'pwd'));
        assert(db.auth('spencer', 'password'));

        testUserAdmin.updateUser(
            'spencer', {customData: {zipCode: 10036}, roles: ["read", "testRole"]}, writeConcern);
        const user = testUserAdmin.getUser('spencer');
        assert.eq(10036, user.customData.zipCode);
        hasAuthzError(db.foo.insert({a: 1}));
        assert.eq(1, db.foo.findOne().a);
        assert.eq(1, db.foo.count());
        assert.doesNotThrow(function() {
            db.getRole('testRole');
        });
        assert.commandFailedWithCode(db.adminCommand('connPoolSync'), ErrorCodes.Unauthorized);

        testUserAdmin.updateUser(
            'spencer', {roles: ["readWrite", {role: 'adminRole', db: 'admin'}]}, writeConcern);
        assert.commandWorked(db.foo.update({}, {$inc: {a: 1}}));
        assert.eq(2, db.foo.findOne().a);
        assert.eq(1, db.foo.count());
        assert.throws(function() {
            db.getRole('testRole');
        });
        assert.commandWorked(db.adminCommand('connPoolSync'));
    })();

    (function testGrantRolesToUser() {
        jsTestLog("Testing grantRolesToUser");

        assert.commandFailedWithCode(db.runCommand({collMod: 'foo'}), ErrorCodes.Unauthorized);

        testUserAdmin.grantRolesToUser('spencer',
                                       [
                                           'readWrite',
                                           'dbAdmin',
                                           {role: 'readWrite', db: 'test'},
                                           {role: 'testRole', db: 'test'},
                                           'readWrite'
                                       ],
                                       writeConcern);

        assert.commandWorked(db.runCommand({collMod: 'foo'}));
        assert.commandWorked(db.foo.update({}, {$inc: {a: 1}}));
        assert.eq(3, db.foo.findOne().a);
        assert.eq(1, db.foo.count());
        assert.doesNotThrow(function() {
            db.getRole('testRole');
        });
        assert.commandWorked(db.adminCommand('connPoolSync'));
    })();

    (function testRevokeRolesFromUser() {
        jsTestLog("Testing revokeRolesFromUser");

        testUserAdmin.revokeRolesFromUser(
            'spencer',
            [
                'readWrite',
                {role: 'dbAdmin', db: 'test2'},  // role user doesnt have
                "testRole"
            ],
            writeConcern);

        assert.commandWorked(db.runCommand({collMod: 'foo'}));
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}));
        assert.throws(function() {
            db.foo.findOne();
        });
        assert.throws(function() {
            db.getRole('testRole');
        });
        assert.commandWorked(db.adminCommand('connPoolSync'));

        testUserAdmin.revokeRolesFromUser(
            'spencer', [{role: 'adminRole', db: 'admin'}], writeConcern);

        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}));
        assert.throws(function() {
            db.foo.findOne();
        });
        assert.throws(function() {
            db.getRole('testRole');
        });
        assert.commandFailedWithCode(db.adminCommand('connPoolSync'), ErrorCodes.Unauthorized);
    })();

    (function testUsersInfo() {
        // Helper functions for the expected output of usersInfo, depending on the variant used.
        function assertNoExtraInfo(user) {
            assert(!user.credentials);
            assertNoPrivilegesOrAuthRestrictions(user);
        }

        function assertNoPrivilegesOrAuthRestrictions(user) {
            assert(!user.inheritedRoles);
            assert(!user.inheritedPrivileges);
            assert(!user.inheritedAuthenticationRestrictions);
            assert(!user.authenticationRestrictions);
        }

        function assertShowCredentials(user) {
            assert(user.credentials['SCRAM-SHA-1']);
            assert(user.credentials['SCRAM-SHA-256']);
        }

        function assertShowPrivileges(user,
                                      expectedInheritedRolesLength,
                                      expectedInheritedPrivilegesLength,
                                      expectedInheritedAuthenticationRestrictionsLength) {
            assert.eq(expectedInheritedRolesLength, user.inheritedRoles.length);
            assert.eq(expectedInheritedPrivilegesLength, user.inheritedPrivileges.length);
            assert.eq(expectedInheritedAuthenticationRestrictionsLength,
                      user.inheritedAuthenticationRestrictions.length);
        }

        function assertShowAuthenticationRestrictions(
            user,
            expectedInheritedRolesLength,
            expectedInheritedPrivilegesLength,
            expectedInheritedAuthenticationRestrictionsLength,
            expectedAuthenticationRestrictionsLength) {
            assertShowPrivileges(user,
                                 expectedInheritedRolesLength,
                                 expectedInheritedPrivilegesLength,
                                 expectedInheritedAuthenticationRestrictionsLength);
            assert.eq(expectedAuthenticationRestrictionsLength,
                      user.authenticationRestrictions.length);
        }

        jsTestLog("Testing usersInfo");

        jsTestLog("Running exact usersInfo with default options on username only");
        let res = testUserAdmin.runCommand({usersInfo: 'spencer'});
        printjson(res);
        assert.eq(1, res.users.length);
        assert.eq(10036, res.users[0].customData.zipCode);
        assertNoExtraInfo(res.users[0]);

        jsTestLog("Running exact usersInfo with default options on username and db");
        res = testUserAdmin.runCommand({usersInfo: {user: 'spencer', db: 'test'}});
        printjson(res);
        assert.eq(1, res.users.length);
        assert.eq(10036, res.users[0].customData.zipCode);
        assertNoExtraInfo(res.users[0]);

        jsTestLog('Running exact usersInfo on single user with showCredentials set to true');
        res = testUserAdmin.runCommand(
            {usersInfo: {user: 'spencer', db: 'test'}, showCredentials: true});
        printjson(res);
        assert.eq(1, res.users.length);
        assert.eq(10036, res.users[0].customData.zipCode);
        assertShowCredentials(res.users[0]);
        assertNoPrivilegesOrAuthRestrictions(res.users[0]);

        jsTestLog('Running exact usersInfo on single user with showPrivileges set to true');
        res = testUserAdmin.runCommand(
            {usersInfo: {user: 'spencer', db: 'test'}, showPrivileges: true});
        printjson(res);
        assert.eq(1, res.users.length);
        assert.eq(10036, res.users[0].customData.zipCode);
        assert(!res.users[0].credentials);
        assertShowPrivileges(res.users[0], 1, 2, 0);
        assert(!res.users[0].authenticationRestrictions);

        jsTestLog(
            'Running exact usersInfo on single user with showAuthenticationRestrictions set to true');
        res = testUserAdmin.runCommand(
            {usersInfo: {user: 'spencer', db: 'test'}, showAuthenticationRestrictions: true});
        printjson(res);
        assert.eq(1, res.users.length);
        assert.eq(10036, res.users[0].customData.zipCode);
        assert(!res.users[0].credentials);
        assertShowAuthenticationRestrictions(res.users[0], 1, 2, 0, 0);

        jsTestLog('Running exact usersInfo on single user with showCustomData set to false');
        res = testUserAdmin.runCommand(
            {usersInfo: {user: 'spencer', db: 'test'}, showCustomData: false});
        printjson(res);
        assert.eq(1, res.users.length);
        assert(!res.users[0].customData);
        assertNoExtraInfo(res.users[0]);

        // This should trigger the authorization user cache.
        jsTestLog('Running exact usersInfo on single user with all non-default options set');
        res = testUserAdmin.runCommand({
            usersInfo: {user: 'spencer', db: 'test'},
            showCredentials: true,
            showPrivileges: true,
            showAuthenticationRestrictions: true,
            showCustomData: false
        });
        printjson(res);
        assert.eq(1, res.users.length);
        assert(!res.users[0].customData);
        assertShowCredentials(res.users[0]);
        assertShowAuthenticationRestrictions(res.users[0], 1, 2, 0, 0);

        // UsersInfo results are ordered alphabetically by user field then db field,
        // not by user insertion order
        jsTestLog('Running exact usersInfo on multiple users with default options');
        res = testUserAdmin.runCommand({usersInfo: ['spencer', {user: 'userAdmin', db: 'admin'}]});
        printjson(res);
        assert.eq(2, res.users.length);
        assert.eq("spencer", res.users[0].user);
        assert.eq(10036, res.users[0].customData.zipCode);
        assert.eq("userAdmin", res.users[1].user);
        assert(res.users[1].customData.userAdmin);
        res.users.forEach(user => {
            assertNoExtraInfo(user);
        });

        jsTestLog('Running exact usersInfo on multiple users with showCredentials set to true');
        res = testUserAdmin.runCommand(
            {usersInfo: ['spencer', {user: 'userAdmin', db: 'admin'}], showCredentials: true});
        printjson(res);
        assert.eq(2, res.users.length);
        assert.eq("spencer", res.users[0].user);
        assert.eq(10036, res.users[0].customData.zipCode);
        assert.eq("userAdmin", res.users[1].user);
        assert(res.users[1].customData.userAdmin);
        res.users.forEach(user => {
            assertShowCredentials(user);
            assertNoPrivilegesOrAuthRestrictions(user);
        });

        jsTestLog('Running exact usersInfo on multiple users with showPrivileges set to true');
        res = testUserAdmin.runCommand(
            {usersInfo: ['spencer', {user: 'userAdmin', db: 'admin'}], showPrivileges: true});
        printjson(res);
        assert.eq(2, res.users.length);
        assert.eq("spencer", res.users[0].user);
        assert.eq(10036, res.users[0].customData.zipCode);
        assert(!res.users[0].credentials);
        assertShowPrivileges(res.users[0], 1, 2, 0);
        assert(!res.users[0].authenticationRestrictions);
        assert.eq("userAdmin", res.users[1].user);
        assert(res.users[1].customData.userAdmin);
        assert(!res.users[1].credentials);
        assertShowPrivileges(res.users[1], 1, 9, 0);
        assert(!res.users[1].authenticationRestrictions);

        jsTestLog(
            'Running exact usersInfo on multiple users with showAuthenticationRestrictions set to true');
        res = testUserAdmin.runCommand({
            usersInfo: ['spencer', {user: 'userAdmin', db: 'admin'}],
            showAuthenticationRestrictions: true
        });
        printjson(res);
        assert.eq(2, res.users.length);
        assert.eq("spencer", res.users[0].user);
        assert.eq(10036, res.users[0].customData.zipCode);
        assert(!res.users[0].credentials);
        assertShowAuthenticationRestrictions(res.users[0], 1, 2, 0, 0);
        assert.eq("userAdmin", res.users[1].user);
        assert(res.users[1].customData.userAdmin);
        assert(!res.users[1].credentials);
        assertShowAuthenticationRestrictions(res.users[1], 1, 9, 0, 0);

        // This should also trigger the authorization user cache.
        jsTestLog('Running exact usersInfo on multiple users with all non-default options set');
        res = testUserAdmin.runCommand({
            usersInfo: ['spencer', {user: 'userAdmin', db: 'admin'}],
            showCredentials: true,
            showPrivileges: true,
            showAuthenticationRestrictions: true,
            showCustomData: false,
        });
        printjson(res);
        assert.eq(2, res.users.length);
        assert.eq("spencer", res.users[0].user);
        assert(!res.users[0].customData);
        assertShowAuthenticationRestrictions(res.users[0], 1, 2, 0, 0);
        assert.eq("userAdmin", res.users[1].user);
        assert(!res.users[1].customData);
        assertShowAuthenticationRestrictions(res.users[1], 1, 9, 0, 0);
        res.users.forEach(user => {
            assertShowCredentials(user);
        });

        jsTestLog('Running non-exact usersInfo on current db with all default options set');
        res = testUserAdmin.runCommand({usersInfo: 1});
        assert.eq(2, res.users.length);
        assert.eq("andy", res.users[0].user);
        assert.eq("spencer", res.users[1].user);
        assert(!res.users[0].customData);
        assert.eq(10036, res.users[1].customData.zipCode);
        // showPrivileges and showAuthenticationRestrictions should not be allowed on non-exact
        // usersInfo queries.
        assert.commandFailed(testUserAdmin.runCommand({usersInfo: 1, showPrivileges: true}));
        assert.commandFailed(
            testUserAdmin.runCommand({usersInfo: 1, showAuthenticationRestrictions: true}));

        // showCredentials and showCustomData should be allowed on non-exact usersInfo queries.
        jsTestLog(
            'Running non-exact usersInfo on current db with showCredentials and showCustomData set to non-defaults');
        res =
            testUserAdmin.runCommand({usersInfo: 1, showCredentials: true, showCustomData: false});
        printjson(res);
        assert.eq(2, res.users.length);
        assert.eq("andy", res.users[0].user);
        assert.eq("spencer", res.users[1].user);
        res.users.forEach(user => {
            assertShowCredentials(user);
            assert(!user.customData);
        });

        res = testUserAdmin.runCommand({usersInfo: {forAllDBs: true}});
        printjson(res);
        assert.eq(4, res.users.length);
        assert.eq("admin", res.users[0].user);
        assert.eq("andy", res.users[1].user);
        assert.eq("spencer", res.users[2].user);
        assert.eq("userAdmin", res.users[3].user);
        // showPrivileges and showAuthenticationRestrictions should not be allowed on non-exact
        // usersInfo queries.
        assert.commandFailed(
            testUserAdmin.runCommand({usersInfo: {forAllDBs: true}, showPrivileges: true}));
        assert.commandFailed(testUserAdmin.runCommand(
            {usersInfo: {forAllDBs: true}, showAuthenticationRestrictions: true}));

        // showCredentials and showCustomData should be allowed on non-exact usersInfo queries.
        res = testUserAdmin.runCommand(
            {usersInfo: {forAllDBs: true}, showCredentials: true, showCustomData: false});
        printjson(res);
        assert.eq(4, res.users.length);
        assert.eq("admin", res.users[0].user);
        assert.eq("andy", res.users[1].user);
        assert.eq("spencer", res.users[2].user);
        assert.eq("userAdmin", res.users[3].user);
        res.users.forEach(user => {
            assertShowCredentials(user);
            assert(!user.customData);
        });
    })();

    (function testDropUser() {
        jsTestLog("Testing dropUser");

        assert(db.auth('spencer', 'password'));
        db.logout();
        assert(db.auth('andy', 'pwd'));

        testUserAdmin.dropUser('spencer', writeConcern);

        assert(!db.auth('spencer', 'password'));
        assert(db.auth('andy', 'pwd'));
        db.logout();

        assert.eq(1, testUserAdmin.getUsers().length);
    })();

    (function testDropAllUsersFromDatabase() {
        jsTestLog("Testing dropAllUsersFromDatabase");

        assert.eq(1, testUserAdmin.getUsers().length);
        assert(db.auth('andy', 'pwd'));
        db.logout();

        testUserAdmin.dropAllUsers(writeConcern);

        assert(!db.auth('andy', 'pwd'));
        assert.eq(0, testUserAdmin.getUsers().length);
    })();
}
