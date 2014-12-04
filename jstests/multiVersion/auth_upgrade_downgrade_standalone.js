// Standalone upgrade / downgrade tests.
load('./jstests/multiVersion/libs/auth_helpers.js');

var legacyVersion = "2.4"
var oldVersion = "2.6"
var newVersion = "2.8"

var conn = MongoRunner.runMongod({binVersion: legacyVersion,
                                  auth: ''});

var test28Shell24Mongod = function(){
    // Test that the 2.8 shell can authenticate a 2.4 mongod.
    var adminDB = conn.getDB('admin');

    // Note - the 2.8 shell no longer includes addUser, so we
    // have to manually create the user here, including the
    // password hash.
    adminDB.system.users.insert({user: 'user1',
                                 pwd: 'e7e8a26d330dbb8bef5b1886ceb5e290'});

    // Test auth with and without explicit mechanism.
    assert(adminDB.auth({user: 'user1', pwd: 'password'}));
    assert(adminDB.auth({mechanism: 'MONGODB-CR',
                         user: 'user1', pwd: 'password'}));
}

var testUpgrade26 = function(){
    // Upgrade mongod to 2.6, do authSchemaUpgrade, add another user.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({restart: conn.runId, binVersion: oldVersion});
    var adminDB = conn.getDB('admin');

    // Test auth with and without explicit mechanism.
    assert(adminDB.auth({user: 'user1', pwd: 'password'}));
    assert(adminDB.auth({mechanism: 'MONGODB-CR',
                         user: 'user1', pwd: 'password'}));

    adminDB.runCommand('authSchemaUpgrade');
    assert(adminDB.auth({mechanism: 'MONGODB-CR',
                         user: 'user1', pwd: 'password'}));

    adminDB.updateUser('user1', {pwd: 'pass',
                                 roles: jsTest.adminUserRoles});
    assert(adminDB.auth({mechanism: 'MONGODB-CR',
                         user: 'user1', pwd: 'pass'}));

    adminDB.createUser({user: 'user2', pwd: 'pass',
                        roles: jsTest.adminUserRoles});
    assert(adminDB.auth({mechanism: 'MONGODB-CR',
                         user: 'user2', pwd: 'pass'}));
}

var testUpgrade28NoSchemaUpgrade = function(){
    // Upgrade mongod to 2.8, test that MONGODB-CR and SCRAM both work,
    // update a user, make sure both still work.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({restart: conn.runId, binVersion: newVersion});
    var adminDB = conn.getDB('admin');
    verifyAuth(adminDB, 'user1', 'pass', true, true);

    // We haven't run authSchemaUpgrade so there shouldn't be
    // any stored SCRAM-SHA-1 credentials.
    verifyUserDoc(adminDB, 'user1', true, false);
    verifyUserDoc(adminDB, 'user2', true, false);

    adminDB.updateUser('user1', {pwd: 'newpass',
                                 roles: jsTest.adminUserRoles});
    verifyAuth(adminDB, 'user1', 'newpass', true, true);
    verifyUserDoc(adminDB, 'user1', true, false);
}

var testDowngrade26PreSchemaUpgrade = function(){
    // Downgrade to 2.6, make sure MONGODB-CR still works.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({restart: conn.runId, binVersion: oldVersion});
    var adminDB = conn.getDB('admin');
    assert(adminDB.auth({user: 'user1', pwd: 'newpass'}));
    assert(adminDB.auth({mechanism: 'MONGODB-CR',
                         user: 'user1', pwd: 'newpass'}));
}

var testUpgrade28WithSchemaUpgrade = function(){
    // Upgrade back to 2.8, do authSchemaUpgrade, verify auth and user docs.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({restart: conn.runId, binVersion: newVersion});
    var adminDB = conn.getDB('admin');
    verifyAuth(adminDB, 'user1', 'newpass', true, true);
    adminDB.runCommand('authSchemaUpgrade');

    // After authSchemaUpgrade MONGODB-CR no longer works.
    verifyAuth(adminDB, 'user1', 'newpass', false, true);
    verifyAuth(adminDB, 'user2', 'pass', false, true);

    // All users should only have SCRAM credentials.
    verifyUserDoc(adminDB, 'user1', false, true);
    verifyUserDoc(adminDB, 'user2', false, true);
}

var testDowngrade26PostSchemaUpgrade = function(){
    // Downgrade to 2.6 again, nothing should work.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({restart: conn.runId, binVersion: oldVersion});
    var adminDB = conn.getDB('admin');
    verifyAuth(adminDB, 'user1', 'pass', false, false);
    verifyAuth(adminDB, 'user2', 'pass', false, false);
}

test28Shell24Mongod();
testUpgrade26();
testUpgrade28NoSchemaUpgrade();
testDowngrade26PreSchemaUpgrade();
testUpgrade28WithSchemaUpgrade();
testDowngrade26PostSchemaUpgrade();

MongoRunner.stopMongod(conn);
