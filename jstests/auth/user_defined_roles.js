/**
 * This tests that user defined roles actually grant users the ability to perform the actions they
 * should, and that changing the privileges assigned to roles changes the access granted to the user
 */

function runTest(conn) {
    var authzErrorCode = 13;
    var hasAuthzError = function(result) {
        assert(result.hasWriteError());
        assert.eq(authzErrorCode, result.getWriteError().code);
    };

    conn.getDB('admin').createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
    conn.getDB('admin').auth('admin', 'pwd');
    conn.getDB('admin').createUser(
        {user: 'userAdmin', pwd: 'pwd', roles: ['userAdminAnyDatabase']});
    conn.getDB('admin').logout();

    var userAdminConn = new Mongo(conn.host);
    var adminUserAdmin = userAdminConn.getDB('admin');
    adminUserAdmin.auth('userAdmin', 'pwd');
    adminUserAdmin.createRole({role: 'adminRole', privileges: [], roles: []});
    var testUserAdmin = userAdminConn.getDB('test');
    testUserAdmin.createRole({role: 'testRole1', privileges: [], roles: []});
    testUserAdmin.createRole({role: 'testRole2', privileges: [], roles: ['testRole1']});
    testUserAdmin.createUser(
        {user: 'testUser', pwd: 'pwd', roles: ['testRole2', {role: 'adminRole', db: 'admin'}]});

    var testDB = conn.getDB('test');
    assert(testDB.auth('testUser', 'pwd'));

    // At this point there are 3 db handles in use.  testUserAdmin and adminUserAdmin are handles to
    // the "test" and "admin" dbs respectively.  Both testUserAdmin and adminUserAdmin are on the
    // same connection (userAdminConn) which has been auth'd as a user with the
    // 'userAdminAnyDatabase' role.  Those will be used for manipulating the user defined roles
    // used in the test.  "testDB" is a handle to the test database on a connection that has been
    // auth'd as 'testUser@test' - this is the connection that will be used to test how privilege
    // enforcement works.

    // test CRUD
    hasAuthzError(testDB.foo.insert({a: 1}));
    assert.throws(function() {
        testDB.foo.findOne();
    });

    testUserAdmin.grantPrivilegesToRole(
        'testRole1', [{resource: {db: 'test', collection: ''}, actions: ['find']}]);

    hasAuthzError(testDB.foo.insert({a: 1}));
    assert.doesNotThrow(function() {
        testDB.foo.findOne();
    });
    assert.eq(0, testDB.foo.count());
    assert.eq(0, testDB.foo.find().itcount());

    testUserAdmin.grantPrivilegesToRole(
        'testRole1', [{resource: {db: 'test', collection: 'foo'}, actions: ['insert']}]);

    assert.writeOK(testDB.foo.insert({a: 1}));
    assert.eq(1, testDB.foo.findOne().a);
    assert.eq(1, testDB.foo.count());
    assert.eq(1, testDB.foo.find().itcount());
    hasAuthzError(testDB.foo.update({a: 1}, {$inc: {a: 1}}));
    assert.eq(1, testDB.foo.findOne().a);

    hasAuthzError(testDB.bar.insert({a: 1}));
    assert.eq(0, testDB.bar.count());

    adminUserAdmin.grantPrivilegesToRole(
        'adminRole', [{resource: {db: '', collection: 'foo'}, actions: ['update']}]);
    assert.writeOK(testDB.foo.update({a: 1}, {$inc: {a: 1}}));
    assert.eq(2, testDB.foo.findOne().a);
    assert.writeOK(testDB.foo.update({b: 1}, {$inc: {b: 1}}, true));  // upsert
    assert.eq(2, testDB.foo.count());
    assert.eq(2, testDB.foo.findOne({b: {$exists: true}}).b);
    hasAuthzError(testDB.foo.remove({b: 2}));
    assert.eq(2, testDB.foo.count());

    adminUserAdmin.grantPrivilegesToRole(
        'adminRole', [{resource: {db: '', collection: ''}, actions: ['remove']}]);
    assert.writeOK(testDB.foo.remove({b: 2}));
    assert.eq(1, testDB.foo.count());

    // Test revoking privileges
    testUserAdmin.revokePrivilegesFromRole(
        'testRole1', [{resource: {db: 'test', collection: 'foo'}, actions: ['insert']}]);
    hasAuthzError(testDB.foo.insert({a: 1}));
    assert.eq(1, testDB.foo.count());
    assert.writeOK(testDB.foo.update({a: 2}, {$inc: {a: 1}}));
    assert.eq(3, testDB.foo.findOne({a: {$exists: true}}).a);
    hasAuthzError(testDB.foo.update({c: 1}, {$inc: {c: 1}}, true));  // upsert should fail
    assert.eq(1, testDB.foo.count());

    // Test changeOwnPassword/changeOwnCustomData
    assert.throws(function() {
        testDB.changeUserPassword('testUser', 'password');
    });
    assert.throws(function() {
        testDB.updateUser('testUser', {customData: {zipCode: 10036}});
    });
    assert.eq(null, testDB.getUser('testUser').customData);
    testUserAdmin.grantPrivilegesToRole('testRole1',
                                        [{
                                           resource: {db: 'test', collection: ''},
                                           actions: ['changeOwnPassword', 'changeOwnCustomData']
                                        }]);
    testDB.changeUserPassword('testUser', 'password');
    assert(!testDB.auth('testUser', 'pwd'));
    assert(testDB.auth('testUser', 'password'));
    testDB.updateUser('testUser', {customData: {zipCode: 10036}});
    assert.eq(10036, testDB.getUser('testUser').customData.zipCode);

    testUserAdmin.revokeRolesFromRole('testRole2', ['testRole1']);
    assert.throws(function() {
        testDB.changeUserPassword('testUser', 'pwd');
    });
    assert.throws(function() {
        testDB.foo.findOne();
    });
    assert.throws(function() {
        testDB.updateUser('testUser', {customData: {zipCode: 10028}});
    });
    assert.eq(10036, testDB.getUser('testUser').customData.zipCode);

    // Test changeAnyPassword/changeAnyCustomData
    testUserAdmin.grantPrivilegesToRole('testRole2', [
        {resource: {db: 'test', collection: ''}, actions: ['changePassword', 'changeCustomData']}
    ]);
    testDB.changeUserPassword('testUser', 'pwd');
    assert(!testDB.auth('testUser', 'password'));
    assert(testDB.auth('testUser', 'pwd'));
    testDB.updateUser('testUser', {customData: {zipCode: 10028}});
    assert.eq(10028, testDB.getUser('testUser').customData.zipCode);

    // Test privileges on the cluster resource
    assert.commandFailed(testDB.runCommand({serverStatus: 1}));
    adminUserAdmin.grantPrivilegesToRole('adminRole',
                                         [{resource: {cluster: true}, actions: ['serverStatus']}]);
    assert.commandWorked(testDB.serverStatus());
}

jsTest.log('Test standalone');
var conn = MongoRunner.runMongod({auth: ''});
runTest(conn);
MongoRunner.stopMongod(conn.port);

jsTest.log('Test sharding');
var st = new ShardingTest({shards: 2, config: 3, keyFile: 'jstests/libs/key1'});
runTest(st.s);
st.stop();
