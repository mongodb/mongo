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
    assert.commandWorked(
        adminDb.runCommand({updateRole: testRole, privileges: testcase.privileges}));
    adminDb.logout();

    assert(adminDb.auth(testUser, "password"));

    var res = runOnDb.runCommand(t.command);

    if (!testcase.expectFail && res.ok != 1 && res.code != commandNotSupportedCode) {
        // don't error if the test failed with code commandNotSupported since
        // some storage engines (e.g wiredTiger) don't support some commands (e.g. touch)
        out = "command failed with " + tojson(res) + " on db " + testcase.runOnDb +
            " with privileges " + tojson(testcase.privileges);
    } else if (testcase.expectFail && res.code == authErrCode) {
        out = "expected authorization success" + " but received " + tojson(res) + " on db " +
            testcase.runOnDb + " with privileges " + tojson(testcase.privileges);
    }

    firstDb.logout();
    authCommandsLib.teardown(conn, t, runOnDb);
    return out;
}

function testInsufficientPrivileges(conn, t, testcase, privileges) {
    var out = "";

    var runOnDb = conn.getDB(testcase.runOnDb);
    var firstDb = conn.getDB(firstDbName);
    var adminDb = conn.getDB(adminDbName);

    authCommandsLib.setup(conn, t, runOnDb);

    adminDb.auth("admin", "password");
    assert.commandWorked(adminDb.runCommand({updateRole: testRole, privileges: privileges}));
    adminDb.logout();

    assert(adminDb.auth(testUser, "password"));

    var res = runOnDb.runCommand(t.command);

    if (res.ok == 1 || res.code != authErrCode) {
        out = "expected authorization failure " + " but received " + tojson(res) +
            " with privileges " + tojson(privileges);
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
        if (!("privileges" in testcase)) {
            continue;
        }
        // Make a copy of the priviliges array since it will be modified.
        var privileges = testcase.privileges.map(function(p) {
            return Object.extend({}, p, true);
        });

        if (testcase.expectAuthzFailure) {
            msg = testInsufficientPrivileges(conn, t, testcase, privileges);
            if (msg) {
                failures.push(t.testname + ": " + msg);
            }
            continue;
        }

        if ((privileges.length == 1 && privileges[0].actions.length > 1) || privileges.length > 1) {
            for (var j = 0; j < privileges.length; j++) {
                var p = privileges[j];
                var resource = p.resource;
                var actions = p.actions;

                for (var k = 0; k < actions.length; k++) {
                    var privDoc = {resource: resource, actions: [actions[k]]};
                    msg = testInsufficientPrivileges(conn, t, testcase, [privDoc]);
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
        privileges.forEach(function(j) {
            if (j.resource.collection && !j.resource.collection.startsWith('system.')) {
                j.resource.collection = "";
            }
        });
        msg = testProperAuthorization(conn, t, testcase);
        if (msg) {
            failures.push(t.testname + ": " + msg);
        }
        // test resource pattern where database is ""
        privileges.forEach(function(j) {
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
    adminDb.createUser({user: "admin", pwd: "password", roles: ["__system"]});

    assert(adminDb.auth("admin", "password"));

    assert.commandWorked(adminDb.runCommand({createRole: testRole, privileges: [], roles: []}));
    assert.commandWorked(adminDb.runCommand(
        {createUser: testUser, pwd: "password", roles: [{role: testRole, db: adminDbName}]}));

    adminDb.logout();
}

var opts = {auth: "", enableExperimentalStorageDetailsCmd: ""};
var impls = {createUsers: createUsers, runOneTest: runOneTest};

// run all tests standalone
var conn = MongoRunner.runMongod(opts);
authCommandsLib.runTests(conn, impls);
MongoRunner.stopMongod(conn);

// run all tests sharded
conn = new ShardingTest(
    {shards: 2, mongos: 1, keyFile: "jstests/libs/key1", other: {shardOptions: opts}});
authCommandsLib.runTests(conn, impls);
conn.stop();
