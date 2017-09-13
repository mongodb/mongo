/**
 * This tests that all the different commands for user manipulation all work properly for all valid
 * forms of input.
 */
function runAllUserManagementCommandsTests(conn, writeConcern) {
    'use strict';

    var hasAuthzError = function(result) {
        assert(result.hasWriteError());
        assert.eq(ErrorCodes.Unauthorized, result.getWriteError().code);
    };

    conn.getDB('admin').createUser({user: 'admin', pwd: 'pwd', roles: ['root']}, writeConcern);
    conn.getDB('admin').auth('admin', 'pwd');
    conn.getDB('admin').createUser({
        user: 'userAdmin',
        pwd: 'pwd',
        roles: ['userAdminAnyDatabase'],
        customData: {userAdmin: true}
    },
                                   writeConcern);
    conn.getDB('admin').logout();

    var userAdminConn = new Mongo(conn.host);
    userAdminConn.getDB('admin').auth('userAdmin', 'pwd');
    var testUserAdmin = userAdminConn.getDB('test');
    testUserAdmin.createRole({
        role: 'testRole',
        roles: [],
        privileges: [{resource: {db: 'test', collection: ''}, actions: ['viewRole']}],
    },
                             writeConcern);
    userAdminConn.getDB('admin').createRole({
        role: 'adminRole',
        roles: [],
        privileges: [{resource: {cluster: true}, actions: ['connPoolSync']}]
    },
                                            writeConcern);

    var db = conn.getDB('test');

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

        var user = testUserAdmin.getUser('spencer');
        assert.eq(10028, user.customData.zipCode);
        assert(db.auth('spencer', 'pwd'));
        assert.writeOK(db.foo.insert({a: 1}));
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
    })();

    (function testUpdateUser() {
        jsTestLog("Testing updateUser");

        testUserAdmin.updateUser('spencer', {pwd: 'password', customData: {}}, writeConcern);
        var user = testUserAdmin.getUser('spencer');
        assert.eq(null, user.customData.zipCode);
        assert(!db.auth('spencer', 'pwd'));
        assert(db.auth('spencer', 'password'));

        testUserAdmin.updateUser(
            'spencer', {customData: {zipCode: 10036}, roles: ["read", "testRole"]}, writeConcern);
        var user = testUserAdmin.getUser('spencer');
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
        assert.writeOK(db.foo.update({}, {$inc: {a: 1}}));
        assert.eq(2, db.foo.findOne().a);
        assert.eq(1, db.foo.count());
        assert.throws(function() {
            db.getRole('testRole');
        });
        assert.commandWorked(db.adminCommand('connPoolSync'));
    })();

    (function testGrantRolesToUser() {
        jsTestLog("Testing grantRolesToUser");

        assert.commandFailedWithCode(db.runCommand({collMod: 'foo', usePowerOf2Sizes: true}),
                                     ErrorCodes.Unauthorized);

        testUserAdmin.grantRolesToUser('spencer',
                                       [
                                         'readWrite',
                                         'dbAdmin',
                                         {role: 'readWrite', db: 'test'},
                                         {role: 'testRole', db: 'test'},
                                         'readWrite'
                                       ],
                                       writeConcern);

        assert.commandWorked(db.runCommand({collMod: 'foo', usePowerOf2Sizes: true}));
        assert.writeOK(db.foo.update({}, {$inc: {a: 1}}));
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

        assert.commandWorked(db.runCommand({collMod: 'foo', usePowerOf2Sizes: true}));
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
        jsTestLog("Testing usersInfo");

        var res = testUserAdmin.runCommand({usersInfo: 'spencer'});
        printjson(res);
        assert.eq(1, res.users.length);
        assert.eq(10036, res.users[0].customData.zipCode);

        res = testUserAdmin.runCommand({usersInfo: {user: 'spencer', db: 'test'}});
        assert.eq(1, res.users.length);
        assert.eq(10036, res.users[0].customData.zipCode);

        // UsersInfo results are ordered alphabetically by user field then db field,
        // not by user insertion order
        res = testUserAdmin.runCommand({usersInfo: ['spencer', {user: 'userAdmin', db: 'admin'}]});
        printjson(res);
        assert.eq(2, res.users.length);
        assert.eq("spencer", res.users[0].user);
        assert.eq(10036, res.users[0].customData.zipCode);
        assert(res.users[1].customData.userAdmin);
        assert.eq("userAdmin", res.users[1].user);

        res = testUserAdmin.runCommand({usersInfo: 1});
        assert.eq(2, res.users.length);
        assert.eq("andy", res.users[0].user);
        assert.eq("spencer", res.users[1].user);
        assert(!res.users[0].customData);
        assert.eq(10036, res.users[1].customData.zipCode);
    })();

    (function testDropUser() {
        jsTestLog("Testing dropUser");

        assert(db.auth('spencer', 'password'));
        assert(db.auth('andy', 'pwd'));

        testUserAdmin.dropUser('spencer', writeConcern);

        assert(!db.auth('spencer', 'password'));
        assert(db.auth('andy', 'pwd'));

        assert.eq(1, testUserAdmin.getUsers().length);
    })();

    (function testDropAllUsersFromDatabase() {
        jsTestLog("Testing dropAllUsersFromDatabase");

        assert.eq(1, testUserAdmin.getUsers().length);
        assert(db.auth('andy', 'pwd'));

        testUserAdmin.dropAllUsers(writeConcern);

        assert(!db.auth('andy', 'pwd'));
        assert.eq(0, testUserAdmin.getUsers().length);
    })();
}
