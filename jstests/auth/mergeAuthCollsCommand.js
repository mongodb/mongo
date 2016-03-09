/**
 * Tests the behavior of the _mergeAuthzCollections command.
 */

function assertUsersAndRolesHaveRole(admin, role) {
    admin.system.users.find().forEach(function(doc) {
        assert.eq(1, doc.roles.length);
        assert.eq(role, doc.roles[0].role);
    });
    admin.system.roles.find().forEach(function(doc) {
        assert.eq(1, doc.roles.length);
        assert.eq(role, doc.roles[0].role);
    });
}
function runTest(conn) {
    var db = conn.getDB('test');
    var admin = conn.getDB('admin');

    jsTestLog("Creating users and roles in temp collections");
    db.createUser({user: 'spencer', pwd: 'pwd', roles: ['read']});
    admin.createUser({user: 'andreas', pwd: 'pwd', roles: ['read']});
    db.createRole({role: 'role1', roles: ['read'], privileges: []});
    admin.createRole({role: 'adminRole1', roles: ['read'], privileges: []});

    // Move the newly created users/roles to the temp collections to be used later by
    // _mergeAuthzCollections
    admin.system.users.find().forEach(function(doc) {
        admin.tempusers.insert(doc);
    });
    admin.system.roles.find().forEach(function(doc) {
        admin.temproles.insert(doc);
    });

    admin.system.users.remove({});
    admin.system.roles.remove({});

    jsTestLog("Creating users and roles that should be overriden by _mergeAuthzCollections");
    db.createUser({user: 'spencer', pwd: 'pwd', roles: ['readWrite']});
    db.createUser({user: 'andy', pwd: 'pwd', roles: ['readWrite']});
    admin.createUser({user: 'andreas', pwd: 'pwd', roles: ['readWrite']});
    db.createRole({role: 'role1', roles: ['readWrite'], privileges: []});
    db.createRole({role: 'role2', roles: ['readWrite'], privileges: []});
    admin.createRole({role: 'adminRole1', roles: ['readWrite'], privileges: []});

    assert.eq(3, admin.system.users.count());
    assert.eq(3, admin.system.roles.count());
    assertUsersAndRolesHaveRole(admin, "readWrite");

    jsTestLog("Overriding existing system.users and system.roles collections");
    assert.commandWorked(admin.runCommand({
        _mergeAuthzCollections: 1,
        tempUsersCollection: 'admin.tempusers',
        tempRolesCollection: 'admin.temproles',
        db: "",
        drop: true
    }));

    assert.eq(2, admin.system.users.count());
    assert.eq(2, admin.system.roles.count());
    assertUsersAndRolesHaveRole(admin, "read");

    admin.system.users.remove({});
    admin.system.roles.remove({});

    jsTestLog("Creating users and roles that should be persist after _mergeAuthzCollections");
    db.createUser({user: 'bob', pwd: 'pwd', roles: ['read']});
    admin.createUser({user: 'george', pwd: 'pwd', roles: ['read']});
    db.createRole({role: 'role3', roles: ['read'], privileges: []});
    admin.createRole({role: 'adminRole2', roles: ['read'], privileges: []});

    assert.eq(2, admin.system.users.count());
    assert.eq(2, admin.system.roles.count());
    assertUsersAndRolesHaveRole(admin, "read");

    jsTestLog("Adding users/roles from temp collections to the existing users/roles");
    assert.commandWorked(admin.runCommand({
        _mergeAuthzCollections: 1,
        tempUsersCollection: 'admin.tempusers',
        tempRolesCollection: 'admin.temproles',
        db: "",
        drop: false
    }));

    assert.eq(4, admin.system.users.count());
    assert.eq(4, admin.system.roles.count());
    assertUsersAndRolesHaveRole(admin, "read");

    jsTestLog("Make sure adding duplicate users/roles fails to change anything if 'drop' is false");

    admin.system.users.remove({});
    admin.system.roles.remove({});

    // Create users/roles with the same names as those in the dump but different roles
    db.createUser({user: 'spencer', pwd: 'pwd', roles: ['readWrite']});
    admin.createUser({user: 'andreas', pwd: 'pwd', roles: ['readWrite']});
    db.createRole({role: 'role1', roles: ['readWrite'], privileges: []});
    admin.createRole({role: 'adminRole1', roles: ['readWrite'], privileges: []});

    assert.eq(2, admin.system.users.count());
    assert.eq(2, admin.system.roles.count());
    assertUsersAndRolesHaveRole(admin, "readWrite");

    // This should succeed but have no effect as every user/role it tries to restore already exists
    assert.commandWorked(admin.runCommand({
        _mergeAuthzCollections: 1,
        tempUsersCollection: 'admin.tempusers',
        tempRolesCollection: 'admin.temproles',
        db: "",
        drop: false
    }));

    assert.eq(2, admin.system.users.count());
    assert.eq(2, admin.system.roles.count());
    assertUsersAndRolesHaveRole(admin, "readWrite");
}

jsTest.log('Test standalone');
var conn = MongoRunner.runMongod({});
runTest(conn);
MongoRunner.stopMongod(conn.port);

jsTest.log('Test sharding');
var st = new ShardingTest({shards: 2, config: 3});
runTest(st.s);
st.stop();
