// Test the db.auth() shell helper.

(function() {
'use strict';

const conn = MongoRunner.runMongod();
const admin = conn.getDB('admin');

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

// Invalid mechanisms shouldn't lead to authentication, but also shouldn't crash.
assert(!admin.auth({mechanism: 'this-mechanism-is-fake', user: 'andy', pwd: 'a'}));
MongoRunner.stopMongod(conn);
})();
