// Test the db.auth() shell helper.

const conn = MongoRunner.runMongod();
const admin = conn.getDB('admin');

const kTestUser = 'andy';
const kTestPassword = 'a';

admin.createUser({user: kTestUser, pwd: kTestPassword, roles: jsTest.adminUserRoles});
assert(admin.auth({user: kTestUser, pwd: kTestPassword}));
assert(admin.logout());

// Try all the ways to call db.auth that uses SCRAM-SHA-1.
assert(admin.auth(kTestUser, kTestPassword));
assert(admin.logout());
assert(admin.auth({user: kTestUser, pwd: kTestPassword}));
assert(admin.logout());
assert(admin.auth({mechanism: 'SCRAM-SHA-1', user: kTestUser, pwd: kTestPassword}));
assert(admin.logout());

// MONGODB-CR is not supported anymore.
assert(!admin.auth({mechanism: 'MONGODB-CR', user: kTestUser, pwd: kTestPassword}));
MongoRunner.stopMongod(conn);

// Invalid mechanisms shouldn't lead to authentication, but also shouldn't crash.
assert(!admin.auth({mechanism: 'this-mechanism-is-fake', user: kTestUser, pwd: kTestPassword}));
MongoRunner.stopMongod(conn);
