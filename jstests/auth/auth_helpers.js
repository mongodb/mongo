// Test the db.auth() shell helper.

(function() {
    'use strict';

    const conn = MongoRunner.runMongod({smallfiles: ""});
    const admin = conn.getDB('admin');

    // In order to test MONGODB-CR we need to "reset" the authSchemaVersion to
    // 26Final "3" or else the user won't get MONGODB-CR credentials.
    admin.system.version.save({"_id": "authSchema", "currentVersion": 3});
    admin.createUser({user: 'andy', pwd: 'a', roles: jsTest.adminUserRoles});
    assert(admin.auth({user: 'andy', pwd: 'a'}));
    assert(admin.logout());

    // Try all the ways to call db.auth that uses SCRAM-SHA-1 or MONGODB-CR.
    assert(admin.auth('andy', 'a'));
    assert(admin.logout());
    assert(admin.auth({user: 'andy', pwd: 'a'}));
    assert(admin.logout());
    assert(admin.auth({mechanism: 'SCRAM-SHA-1', user: 'andy', pwd: 'a'}));
    assert(admin.logout());
    assert(admin.auth({mechanism: 'MONGODB-CR', user: 'andy', pwd: 'a'}));
    assert(admin.logout());

    // Invalid mechanisms shouldn't lead to authentication, but also shouldn't crash.
    assert(!admin.auth({mechanism: 'this-mechanism-is-fake', user: 'andy', pwd: 'a'}));
})();
