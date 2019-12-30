// Ensure that attempting to use SCRAM-SHA-1 auth on a
// user with invalid SCRAM-SHA-1 credentials fails gracefully.

(function() {
'use strict';

const mongod = MongoRunner.runMongod({auth: "", useLogFiles: true});
const admin = mongod.getDB('admin');
const test = mongod.getDB('test');

admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
assert(admin.auth('admin', 'pass'));

test.createUser({user: 'user', pwd: 'pass', roles: jsTest.basicUserRoles});

// Give the test user an invalid set of SCRAM-SHA-1 credentials.
assert.eq(
    admin.system.users
        .update({_id: "test.user"}, {
            $set: {
                "credentials.SCRAM-SHA-1":
                    {salt: "AAAA", storedKey: "AAAA", serverKey: "AAAA", iterationCount: 10000}
            }
        })
        .nModified,
    1,
    "Should have updated one document for user@test");
admin.logout();

const error = assert.throws(function() {
    test._authOrThrow({user: 'user', pwd: 'pass'});
});

assert.eq(error, "Error: Authentication failed.");

MongoRunner.stopMongod(mongod);
})();
