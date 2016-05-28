/**
 * This tests that all the different commands for user manipulation all work properly for all valid
 * forms of input.
 */

function runTest(conn) {
    var authzErrorCode = 13;
    var hasAuthzError = function(result) {
        assert(result.hasWriteError());
        assert.eq(authzErrorCode, result.getWriteError().code);
    };

    conn.getDB('admin').createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
    conn.getDB('admin').auth('admin', 'pwd');
    conn.getDB('admin').createUser({
        user: 'userAdmin',
        pwd: 'pwd',
        roles: ['userAdminAnyDatabase'],
        customData: {userAdmin: true}
    });
    conn.getDB('admin').logout();

    var userAdminConn = new Mongo(conn.host);
    userAdminConn.getDB('admin').auth('userAdmin', 'pwd');
    var testUserAdmin = userAdminConn.getDB('test');
    testUserAdmin.createRole({
        role: 'testRole',
        roles: [],
        privileges: [{resource: {db: 'test', collection: ''}, actions: ['viewRole']}],
    });
    userAdminConn.getDB('admin').createRole({
        role: 'adminRole',
        roles: [],
        privileges: [{resource: {cluster: true}, actions: ['connPoolSync']}]
    });

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
        });
        testUserAdmin.createUser({user: "andy", pwd: "pwd", roles: []});

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

        testUserAdmin.updateUser('spencer', {pwd: 'password', customData: {}});
        var user = testUserAdmin.getUser('spencer');
        assert.eq(null, user.customData.zipCode);
        assert(!db.auth('spencer', 'pwd'));
        assert(db.auth('spencer', 'password'));

        testUserAdmin.updateUser('spencer',
                                 {customData: {zipCode: 10036}, roles: ["read", "testRole"]});
        var user = testUserAdmin.getUser('spencer');
        assert.eq(10036, user.customData.zipCode);
        hasAuthzError(db.foo.insert({a: 1}));
        assert.eq(1, db.foo.findOne().a);
        assert.eq(1, db.foo.count());
        assert.doesNotThrow(function() {
            db.getRole('testRole');
        });
        assert.commandFailedWithCode(db.adminCommand('connPoolSync'), authzErrorCode);

        testUserAdmin.updateUser('spencer',
                                 {roles: ["readWrite", {role: 'adminRole', db: 'admin'}]});
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
                                     authzErrorCode);

        testUserAdmin.grantRolesToUser('spencer', [
            'readWrite',
            'dbAdmin',
            {role: 'readWrite', db: 'test'},
            {role: 'testRole', db: 'test'},
            'readWrite'
        ]);

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

        testUserAdmin.revokeRolesFromUser('spencer', [
            'readWrite',
            {role: 'dbAdmin', db: 'test2'},  // role user doesnt have
            "testRole"
        ]);

        assert.commandWorked(db.runCommand({collMod: 'foo', usePowerOf2Sizes: true}));
        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}));
        assert.throws(function() {
            db.foo.findOne();
        });
        assert.throws(function() {
            db.getRole('testRole');
        });
        assert.commandWorked(db.adminCommand('connPoolSync'));

        testUserAdmin.revokeRolesFromUser('spencer', [{role: 'adminRole', db: 'admin'}]);

        hasAuthzError(db.foo.update({}, {$inc: {a: 1}}));
        assert.throws(function() {
            db.foo.findOne();
        });
        assert.throws(function() {
            db.getRole('testRole');
        });
        assert.commandFailedWithCode(db.adminCommand('connPoolSync'), authzErrorCode);

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

        res = testUserAdmin.runCommand({usersInfo: ['spencer', {user: 'userAdmin', db: 'admin'}]});
        printjson(res);
        assert.eq(2, res.users.length);
        if (res.users[0].user == "spencer") {
            assert.eq(10036, res.users[0].customData.zipCode);
            assert(res.users[1].customData.userAdmin);
        } else if (res.users[0].user == "userAdmin") {
            assert.eq(10036, res.users[1].customData.zipCode);
            assert(res.users[0].customData.userAdmin);
        } else {
            doassert(
                "Expected user names returned by usersInfo to be either 'userAdmin' or 'spencer', " +
                "but got: " + res.users[0].user);
        }

        res = testUserAdmin.runCommand({usersInfo: 1});
        assert.eq(2, res.users.length);
        if (res.users[0].user == "spencer") {
            assert.eq("andy", res.users[1].user);
            assert.eq(10036, res.users[0].customData.zipCode);
            assert(!res.users[1].customData);
        } else if (res.users[0].user == "andy") {
            assert.eq("spencer", res.users[1].user);
            assert(!res.users[0].customData);
            assert.eq(10036, res.users[1].customData.zipCode);
        } else {
            doassert(
                "Expected user names returned by usersInfo to be either 'andy' or 'spencer', " +
                "but got: " + res.users[0].user);
        }

    })();

    (function testDropUser() {
        jsTestLog("Testing dropUser");

        assert(db.auth('spencer', 'password'));
        assert(db.auth('andy', 'pwd'));

        assert.commandWorked(testUserAdmin.runCommand({dropUser: 'spencer'}));

        assert(!db.auth('spencer', 'password'));
        assert(db.auth('andy', 'pwd'));

        assert.eq(1, testUserAdmin.getUsers().length);
    })();

    (function testDropAllUsersFromDatabase() {
        jsTestLog("Testing dropAllUsersFromDatabase");

        assert.eq(1, testUserAdmin.getUsers().length);
        assert(db.auth('andy', 'pwd'));

        assert.commandWorked(testUserAdmin.runCommand({dropAllUsersFromDatabase: 1}));

        assert(!db.auth('andy', 'pwd'));
        assert.eq(0, testUserAdmin.getUsers().length);
    })();
}

jsTest.log('Test standalone');
var conn = MongoRunner.runMongod({auth: ''});
conn.getDB('admin').runCommand({setParameter: 1, newCollectionsUsePowerOf2Sizes: false});
runTest(conn);
MongoRunner.stopMongod(conn.port);

jsTest.log('Test sharding');
var st = new ShardingTest({shards: 2, config: 3, keyFile: 'jstests/libs/key1'});
runTest(st.s);
st.stop();
