/*

Exhaustive test for authorization of commands with user-defined roles.

The test logic implemented here operates on the test cases defined
in jstests/auth/commands.js.

*/

// constants
var testUser = "userDefinedRolesTestUser";
var testRole = "userDefinedRolesTestRole";

load("jstests/auth/lib/commands_lib.js");

/**
 * Run the command specified in 't' with the privileges specified in 'privileges'.
 */
function testProperAuthorization(conn, t, testcase, privileges) {
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

    if (!testcase.expectFail && res.ok != 1 && res.code != commandNotSupportedCode) {
        // don't error if the test failed with code commandNotSupported since
        // some storage engines (e.g wiredTiger) don't support some commands (e.g. touch)
        out = "command failed with " + tojson(res) + " on db " + testcase.runOnDb +
            " with privileges " + tojson(privileges);
    } else if (testcase.expectFail && res.code == authErrCode) {
        out = "expected authorization success" + " but received " + tojson(res) + " on db " +
            testcase.runOnDb + " with privileges " + tojson(privileges);
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

        if (testcase.expectAuthzFailure) {
            msg = testInsufficientPrivileges(conn, t, testcase, testcase.privileges);
            if (msg) {
                failures.push(t.testname + ": " + msg);
            }
            continue;
        }

        if ((testcase.privileges.length == 1 && testcase.privileges[0].actions.length > 1) ||
            testcase.privileges.length > 1) {
            for (var j = 0; j < testcase.privileges.length; j++) {
                var p = testcase.privileges[j];
                var resource = p.resource;
                var actions = p.actions;

                // A particular privilege can explicitly specify that it should not be removed when
                // testing for authorization failure. This accommodates special-case behavior for
                // views in conjunction with the create and collMod commands.
                if (p.removeWhenTestingAuthzFailure === false) {
                    continue;
                }

                for (var k = 0; k < actions.length; k++) {
                    var privDoc = {resource: resource, actions: [actions[k]]};
                    msg = testInsufficientPrivileges(conn, t, testcase, [privDoc]);
                    if (msg) {
                        failures.push(t.testname + ": " + msg);
                    }
                }
            }
        }

        // Test for proper authorization with the privileges specified in the test case.
        msg = testProperAuthorization(conn, t, testcase, testcase.privileges);
        if (msg) {
            failures.push(t.testname + ": " + msg);
        }

        var specialResource = function(resource) {
            if (!resource)
                return true;

            // Tests which use {db: "local", collection: "oplog.rs"} will not work with
            // {db: "", collection: "oplog.rs"}. oplog.rs is special, and does not match with
            // forDatabaseName or anyNormalResource ResourcePatterns. The same is true of
            // oplog.$main, but oplog.$main is also an illegal collection name on any database
            // other than local. The other collections checked for here in the local database have
            // the same property as oplog.rs.
            return !resource.db || !resource.collection ||
                resource.collection.startsWith("system.") || resource.db == "local";
        };

        // Test for proper authorization with the test case's privileges where non-system
        // collections are modified to be the empty string.
        msg = testProperAuthorization(conn, t, testcase, testcase.privileges.map(function(priv) {
            // Make a copy of the privilege so as not to modify the original array.
            var modifiedPrivilege = Object.extend({}, priv, true);
            if (modifiedPrivilege.resource.collection && !specialResource(priv.resource)) {
                modifiedPrivilege.resource.collection = "";
            }
            return modifiedPrivilege;
        }));
        if (msg) {
            failures.push(t.testname + ": " + msg);
        }

        // Test for proper authorization with the test case's privileges where the database is the
        // empty string.
        msg = testProperAuthorization(conn, t, testcase, testcase.privileges.map(function(priv) {
            // Make a copy of the privilege so as not to modify the original array.
            var modifiedPrivilege = Object.extend({}, priv, true);
            if (!specialResource(priv.resource)) {
                modifiedPrivilege.resource.db = "";
            }
            return modifiedPrivilege;
        }));
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
