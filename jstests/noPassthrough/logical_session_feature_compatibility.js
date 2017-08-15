(function() {
    'use strict';
    var cases = [
        {
          name: "endSessions",
          cmd: {endSessions: []},
        },
        {
          name: "killAllSessions",
          cmd: {killSessions: []},
        },
        {
          name: "killAllSessionsByPattern",
          cmd: {killAllSessionsByPattern: []},
        },
        {
          name: "killSessions",
          cmd: {killSessions: []},
        },
        {
          name: "refreshLogicalSessionCacheNow",
          cmd: {refreshLogicalSessionCacheNow: 1},
        },
        {
          name: "refreshSessions",
          cmd: {refreshSessions: []},
        },
        {
          name: "startSession",
          cmd: {startSession: 1},
        },
    ];

    var casesLength = cases.length;
    var testCases = function(compatibilityVersion, errorCode) {
        assert.commandWorked(
            admin.adminCommand({setFeatureCompatibilityVersion: compatibilityVersion}));
        for (var i = 0; i < casesLength; i++) {
            var testCase = cases[i];
            var result = admin.runCommand(testCase.cmd);

            if (!errorCode) {
                assert.commandWorked(result,
                                     "failed test that we can run " + testCase.name +
                                         " under featureCompatibilityVersion 3.6");

            } else {
                assert.commandFailedWithCode(result,
                                             errorCode,
                                             "failed test that we can't run " + testCase.name +
                                                 " under featureCompatibilityVersion 3.4");
            }
        }
    };

    // First verify the commands without auth, in feature compatibility version 3.6.

    var conn = MongoRunner.runMongod({nojournal: ""});
    var admin = conn.getDB("admin");

    testCases("3.6");

    // Second verify the commands without auth, in feature compatibility version 3.4.

    testCases("3.4", ErrorCodes.InvalidOptions);

    // Third, verify the commands with auth, in feature compatibility version 3.6

    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({auth: "", nojournal: ""});
    admin = conn.getDB("admin");
    admin.createUser({user: 'user0', pwd: 'password', roles: ["root"]});
    admin.auth("user0", "password");

    testCases("3.6");

    // Third, verify the commands with auth, in feature compatibility version 3.4

    testCases("3.4", ErrorCodes.InvalidOptions);

    MongoRunner.stopMongod(conn);

    // Verify that sessions are vivified for ops that have lsids when fcv is 3.4

    var conn = MongoRunner.runMongod({nojournal: ""});
    var admin = conn.getDB("admin");

    assert.commandWorked(admin.adminCommand({setFeatureCompatibilityVersion: "3.6"}));

    for (var i = 0; i < 11; i++) {
        admin.runCommand({"insert": "test", "documents": [{a: 1}], "lsid": {"id": UUID()}});
    }

    admin.adminCommand({setFeatureCompatibilityVersion: "3.4"});

    for (var i = 0; i < 13; i++) {
        admin.runCommand({"insert": "test", "documents": [{a: 2}], "lsid": {"id": UUID()}});
    }

    admin.adminCommand({setFeatureCompatibilityVersion: "3.6"});

    admin.runCommand({refreshLogicalSessionCacheNow: 1});

    assert.eq(admin.system.sessions.find().count(), 11);

})();
