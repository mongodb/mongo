/**
 * This tests that all the different commands for user manipulation all properly handle invalid and
 * atypical inputs.
 */

function runTest(conn) {
    var db = conn.getDB('test');
    var admin = conn.getDB('admin');
    admin.createUser({user: 'userAdmin', pwd: 'pwd', roles: ['userAdminAnyDatabase']});
    admin.auth('userAdmin', 'pwd');

    (function testCreateUser() {
        jsTestLog("Testing createUser");

        db.createUser({user: 'user1', pwd: 'pwd', roles: []});

        // Try to create duplicate user
        assert.throws(function() {
            db.createUser({user: 'user1', pwd: 'pwd', roles: ['read']});
        });
        assert.eq(0, db.getUser('user1').roles.length);

        // Try to create user with role that doesn't exist
        assert.throws(function() {
            db.createUser({user: 'user2', pwd: 'pwd', roles: ['fakeRole']});
        });

        // Try to create user with invalid arguments
        assert.throws(function() {
            db.createUser({user: '', pwd: 'pwd', roles: ['read']});
        });
        assert.throws(function() {
            db.createUser({user: ['user2'], pwd: 'pwd', roles: ['read']});
        });
        assert.throws(function() {
            db.createUser({user: 'user2', pwd: '', roles: ['read']});
        });
        assert.throws(function() {
            db.createUser({user: 'user2', pwd: ['pwd'], roles: ['read']});
        });
        assert.throws(function() {
            db.createUser({user: 'user2', pwd: 'pwd', roles: ['']});
        });
        assert.throws(function() {
            db.createUser({user: 'user2', pwd: 'pwd', roles: [{}]});
        });
        assert.throws(function() {
            db.createUser({user: 'user2', pwd: 'pwd', roles: [1]});
        });
        assert.throws(function() {
            db.createUser({user: 'user2', pwd: 'pwd', roles: [{role: 'read'}]});
        });
        assert.throws(function() {
            db.createUser({user: 'user2', pwd: 'pwd', roles: [{db: 'test'}]});
        });
        assert.throws(function() {
            db.createUser({user: 'user2', pwd: 'pwd', roles: [{role: 'read', db: ''}]});
        });
        assert.throws(function() {
            db.createUser({user: 'user2', pwd: 'pwd', roles: [{role: '', db: 'test'}]});
        });
        assert.throws(function() {
            db.createUser({user: 'null\u0000char', pwd: 'pwd', roles: []});
        });
        assert.throws(function() {
            db.createUser({user: 'null\0char', pwd: 'pwd', roles: []});
        });
        // Regression test for SERVER-17125
        assert.throws(function() {
            db.getSiblingDB('$external').createUser({user: '', roles: []});
        });

        assert.eq(1, db.getUsers().length);
    })();

    (function testUpdateUser() {
        jsTestLog("Testing updateUser");

        // Must update something
        assert.throws(function() {
            db.updateUser('user1', {});
        });

        // Try to grant role that doesn't exist
        assert.throws(function() {
            db.updateUser('user1', {roles: ['fakeRole']});
        });

        // Try to update user that doesn't exist
        assert.throws(function() {
            db.updateUser('fakeUser', {roles: ['read']});
        });

        // Try to update user with invalid password
        assert.throws(function() {
            db.updateUser('user1', {pwd: ''});
        });
        assert.throws(function() {
            db.updateUser('user1', {pwd: 5});
        });
        assert.throws(function() {
            db.updateUser('user1', {pwd: ['a']});
        });

        // Try to update user with invalid customData
        assert.throws(function() {
            db.updateUser('user1', {customData: 1});
        });
        assert.throws(function() {
            db.updateUser('user1', {customData: ""});
        });

        // Try to update with invalid "roles" argument
        assert.throws(function() {
            db.updateUser('user1', {roles: 'read'});
        });
        assert.throws(function() {
            db.updateUser('user1', {roles: ['']});
        });
        assert.throws(function() {
            db.updateUser('user1', {roles: [{}]});
        });
        assert.throws(function() {
            db.updateUser('user1', {roles: [1]});
        });
        assert.throws(function() {
            db.updateUser('user1', {roles: [{role: 'read'}]});
        });
        assert.throws(function() {
            db.updateUser('user1', {roles: [{db: 'test'}]});
        });
        assert.throws(function() {
            db.updateUser('user1', {roles: [{role: '', db: 'test'}]});
        });
        assert.throws(function() {
            db.updateUser('user1', {roles: [{role: 'read', db: ''}]});
        });

        assert.eq(0, db.getUser('user1').roles.length);
    })();

    (function testGrantRolesToUser() {
        jsTestLog("Testing grantRolesToUser");

        // Try to grant role that doesn't exist
        assert.throws(function() {
            db.grantRolesToUser('user1', {roles: ['fakeRole']});
        });

        // Try to grant to user that doesn't exist
        assert.throws(function() {
            db.grantRolesToUser('fakeUser', {roles: ['read']});
        });

        // Must grant something
        assert.throws(function() {
            db.grantRolesToUser('user1', []);
        });

        // Try to grant with invalid arguments
        assert.throws(function() {
            db.grantRolesToUser('user1', 1);
        });
        assert.throws(function() {
            db.grantRolesToUser('user1', [{}]);
        });
        assert.throws(function() {
            db.grantRolesToUser('user1', [1]);
        });
        assert.throws(function() {
            db.grantRolesToUser('user1', 'read');
        });
        assert.throws(function() {
            db.grantRolesToUser('user1', [{role: 'read'}]);
        });
        assert.throws(function() {
            db.grantRolesToUser('user1', [{db: 'test'}]);
        });
        assert.throws(function() {
            db.grantRolesToUser('user1', [{role: 'read', db: ''}]);
        });
        assert.throws(function() {
            db.grantRolesToUser('user1', [{role: '', db: 'test'}]);
        });

        assert.eq(0, db.getUser('user1').roles.length);
        assert.eq(null, db.getUser('user1').customData);
        // Make sure password didn't change
        assert(new Mongo(db.getMongo().host).getDB(db.getName()).auth('user1', 'pwd'));
    })();

    (function testRevokeRolesFromUser() {
        jsTestLog("Testing revokeRolesFromUser");

        // Revoking a role the user doesn't have should succeed but do nothing
        db.revokeRolesFromUser('user1', ['read']);

        // Try to revoke role that doesn't exist
        assert.throws(function() {
            db.revokeRolesFromUser('user1', {roles: ['fakeRole']});
        });

        // Try to revoke from user that doesn't exist
        assert.throws(function() {
            db.revokeRolesFromUser('fakeUser', {roles: ['read']});
        });

        // Must revoke something
        assert.throws(function() {
            db.revokeRolesFromUser('user1', []);
        });

        // Try to revoke with invalid arguments
        assert.throws(function() {
            db.revokeRolesFromUser('user1', 1);
        });
        assert.throws(function() {
            db.revokeRolesFromUser('user1', [{}]);
        });
        assert.throws(function() {
            db.revokeRolesFromUser('user1', [1]);
        });
        assert.throws(function() {
            db.revokeRolesFromUser('user1', 'read');
        });
        assert.throws(function() {
            db.revokeRolesFromUser('user1', [{role: 'read'}]);
        });
        assert.throws(function() {
            db.revokeRolesFromUser('user1', [{db: 'test'}]);
        });
        assert.throws(function() {
            db.revokeRolesFromUser('user1', [{role: 'read', db: ''}]);
        });
        assert.throws(function() {
            db.revokeRolesFromUser('user1', [{role: '', db: 'test'}]);
        });

        assert.eq(0, db.getUser('user1').roles.length);
    })();

    (function testUsersInfo() {
        jsTestLog("Testing usersInfo");

        // Try to get user that does not exist
        assert.eq(null, db.getUser('fakeUser'));

        // Pass wrong type for user name
        assert.throws(function() {
            db.getUser(5);
        });

        assert.throws(function() {
            db.getUser([]);
        });

        assert.throws(function() {
            db.getUser(['user1']);
        });

    })();

    (function testDropUser() {
        jsTestLog("Testing dropUser");

        // Try to drop a user that doesn't exist
        // Should not error but should do nothing
        assert.doesNotThrow(function() {
            db.dropUser('fakeUser');
        });

        assert.eq(1, db.getUsers().length);
    })();

    // dropAllUsersFromDatabase ignores its arguments, so there's nothing to test for it.
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
