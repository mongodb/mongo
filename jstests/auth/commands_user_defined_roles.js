/*

Exhaustive test for authorization of commands with user-defined roles.

The test logic implemented here operates on the test cases defined
in jstests/auth/commands.js.

*/

// constants
var testUser = "userDefinedRolesTestUser";
var testRole = "userDefinedRolesTestRole";

load("jstests/auth/lib/commands_lib.js");

function testProperAuthorization(conn, t, testcase) {
    var out = "";

    var runOnDb = conn.getDB(testcase.runOnDb);
    var firstDb = conn.getDB(firstDbName);
    var adminDb = conn.getDB(adminDbName);

    authCommandsLib.setup(conn, t, runOnDb);

    adminDb.auth("admin", "password");
    assert.commandWorked(firstDb.runCommand({
        updateRole: testRole,
        privileges: testcase.requiredPrivileges
    }));
    adminDb.logout();

    assert(firstDb.auth(testUser, "password"));

    var res = runOnDb.runCommand(t.command);

    if (!testcase.expectFail && res.ok != 1) {
        out = "command failed with " + tojson(res) +
              " on db " + testcase.runOnDb +
              " with privileges " + tojson(testcase.requiredPrivileges);
    }
    else if (testcase.expectFail && res.code == authErrCode) {
            out = "expected authorization success" +
                  " but received " + tojson(res) + 
                  " on db " + testcase.runOnDb +
                  " with privileges " + tojson(testcase.requiredPrivileges);
    }

    firstDb.logout();
    authCommandsLib.teardown(conn, t, runOnDb);
    return out;
}

function testInsufficientPrivilege(conn, t, testcase, privilege) {
    var out = "";

    var runOnDb = conn.getDB(testcase.runOnDb);
    var firstDb = conn.getDB(firstDbName);
    var adminDb = conn.getDB(adminDbName);

    authCommandsLib.setup(conn, t, runOnDb);

    adminDb.auth("admin", "password");
    assert.commandWorked(firstDb.runCommand({
        updateRole: testRole,
        privileges: [ privilege ]
    }));
    adminDb.logout();

    assert(firstDb.auth(testUser, "password"));

    var res = runOnDb.runCommand(t.command);

    if (res.ok == 1 || res.code != authErrCode) {
        out = "expected authorization failure " +
              " but received " + tojson(res) +
              " with privilege " + tojson(privilege);
    }

    firstDb.logout();
    authCommandsLib.teardown(conn, t, runOnDb);
    return out;
}

function runOneTest(conn, t) {
    var failures = [];
    var msg;

    for (var i = 0; i < t.testcases.length; i++) {
        var testcase = t.testcases[i];
        var privileges = testcase.requiredPrivileges;

        if (!("requiredPrivileges" in testcase)) {
            continue;
        }
        else if ((privileges.length == 1 && privileges[0].actions.length > 1)
                 || privileges.length > 1) {
            for (var j = 0; j < privileges.length; j++) {
                var p = privileges[j];
                var resource = p.resource;
                var actions = p.actions;

                for (var k = 0; k < actions.length; k++) {
                    var privDoc = { resource: resource, actions: [actions[k]] };
                    msg = testInsufficientPrivilege(conn, t, testcase, privDoc);
                    if (msg) {
                        failures.push(t.testname + ": " + msg);
                    }
                }
            }
        }

        msg = testProperAuthorization(conn, t, testcase);
        if (msg) {
            failures.push(t.testname + ": " + msg);
        }

        // test resource pattern where collection is ""
        testcase.requiredPrivileges.forEach(function(j) {
            if (j.resource.collection) {
                j.resource.collection = "";
            }
        });
        msg = testProperAuthorization(conn, t, testcase);
        if (msg) {
            failures.push(t.testname + ": " + msg);
        }

        // test resource pattern where database is ""
        testcase.requiredPrivileges.forEach(function(j) {
            if (j.resource.db) {
                j.resource.db = "";
            }
        });
        msg = testProperAuthorization(conn, t, testcase);
        if (msg) {
            failures.push(t.testname + ": " + msg);
        }
    }

    return failures;
}

function createUsers(conn) {
    var adminDb = conn.getDB(adminDbName);
    var firstDb = conn.getDB(firstDbName);
    adminDb.addUser({
        user: "admin",
        pwd: "password",
        roles: ["__system"]
    });

    assert(adminDb.auth("admin", "password"));

    assert.commandWorked(firstDb.runCommand({
        createRole: testRole,
        privileges: [ ],
        roles: [ ]
    }));
    assert.commandWorked(firstDb.runCommand({
        createUser: testUser,
        pwd: "password",
        roles: [ { role: testRole, db: firstDbName } ]
    }));

    adminDb.logout();
}

var opts = {
    auth:"",
    enableExperimentalIndexStatsCmd: "",
    enableExperimentalStorageDetailsCmd: ""
}
var impls = {
    createUsers: createUsers,
    runOneTest: runOneTest
}

// run all tests standalone
var conn = MongoRunner.runMongod(opts);
authCommandsLib.runTests(conn, impls);
MongoRunner.stopMongod(conn);

// run all tests sharded
conn = new ShardingTest({
    shards: 2,
    mongos: 1,
    keyFile: "jstests/libs/key1",
    other: { shardOptions: opts }
});
authCommandsLib.runTests(conn, impls);
conn.stop();

