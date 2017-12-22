// Ensure that attempting to use SCRAM-SHA-1 auth on a
// user with invalid SCRAM-SHA-1 credentials fails gracefully.

(function() {
    'use strict';

    function runTest(mongod) {
        assert(mongod);
        const admin = mongod.getDB('admin');
        const test = mongod.getDB('test');

        admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
        assert(admin.auth('admin', 'pass'));

        test.createUser({user: 'user', pwd: 'pass', roles: jsTest.basicUserRoles});

        // Give the test user an invalid set of SCRAM-SHA-1 credentials.
        assert.eq(admin.system.users
                      .update({_id: "test.user"}, {
                          $set: {
                              "credentials.SCRAM-SHA-1": {
                                  salt: "AAAA",
                                  storedKey: "AAAA",
                                  serverKey: "AAAA",
                                  iterationCount: 10000
                              }
                          }
                      })
                      .nModified,
                  1,
                  "Should have updated one document for user@test");
        admin.logout();

        assert(!test.auth({user: 'user', pwd: 'pass'}));

        assert.soon(function() {
            const log = cat(mongod.fullOptions.logFile);
            return /Unable to perform SCRAM-SHA-1 auth.* invalid SCRAM credentials/.test(log);
        }, "No warning issued for invalid SCRAM-SHA-1 credendials doc", 30 * 1000, 5 * 1000);
    }

    const mongod = MongoRunner.runMongod({auth: "", useLogFiles: true});
    runTest(mongod);
    MongoRunner.stopMongod(mongod);
})();
