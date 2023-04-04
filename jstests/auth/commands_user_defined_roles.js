/*

Exhaustive test for authorization of commands with user-defined roles.

The test logic implemented here operates on the test cases defined
in jstests/auth/lib/commands_lib.js.

@tags: [requires_sharding]

*/

import {
    adminDbName,
    authCommandsLib,
    authErrCode,
    commandNotSupportedCode
} from "jstests/auth/lib/commands_lib.js";

load("jstests/libs/fail_point_util.js");

// This test involves killing all sessions, which will not work as expected if the kill command is
// sent with an implicit session.
TestData.disableImplicitSessions = true;

// constants
var testUser = "userDefinedRolesTestUser";
var testRole = "userDefinedRolesTestRole";

function doTestSetup(conn, t, testcase, privileges) {
    const admin = conn.getDB('admin');
    const runOnDb = conn.getDB(testcase.runOnDb);
    const state = authCommandsLib.setup(conn, t, runOnDb);

    assert(admin.auth('admin', 'password'));
    assert.commandWorked(admin.runCommand({updateRole: testRole, privileges: privileges}));
    admin.logout();

    return state;
}

function doTestTeardown(conn, t, testcase, res) {
    const runOnDb = conn.getDB(testcase.runOnDb);
    authCommandsLib.teardown(conn, t, runOnDb, res);
}

/**
 * Run the command specified in 't' with the privileges specified in 'privileges'.
 */
function testProperAuthorization(conn, t, testcase, privileges) {
    const runOnDb = conn.getDB(testcase.runOnDb);
    const state = doTestSetup(conn.sidechannel, t, testcase, privileges);
    authCommandsLib.authenticatedSetup(t, runOnDb);

    let command = t.command;
    if (typeof (command) === "function") {
        command = t.command(state, testcase.commandArgs);
    }
    const res = runOnDb.runCommand(command);

    let out = "";
    if (!testcase.expectFail && (res.ok != 1) && (res.code != commandNotSupportedCode)) {
        // don't error if the test failed with code commandNotSupported since
        // some storage engines don't support some commands.
        out = "command failed with " + tojson(res) + " on db " + testcase.runOnDb +
            " with privileges " + tojson(privileges);
    } else if (testcase.expectFail && res.code == authErrCode) {
        out = "expected authorization success" +
            " but received " + tojson(res) + " on db " + testcase.runOnDb + " with privileges " +
            tojson(privileges);
    }

    doTestTeardown(conn.sidechannel, t, testcase, res);
    return out;
}

function testInsufficientPrivileges(conn, t, testcase, privileges) {
    const runOnDb = conn.getDB(testcase.runOnDb);
    const state = doTestSetup(conn.sidechannel, t, testcase, privileges);
    authCommandsLib.authenticatedSetup(t, runOnDb);

    let command = t.command;
    if (typeof (command) === "function") {
        command = t.command(state, testcase.commandArgs);
    }
    const res = runOnDb.runCommand(command);

    let out = "";
    if ((res.ok == 1) || (res.code != authErrCode)) {
        out = "expected authorization failure " +
            " but received " + tojson(res) + " with privileges " + tojson(privileges);
    }

    doTestTeardown(conn.sidechannel, t, testcase, res);
    return out;
}

function runOneTest(conn, t) {
    const failures = [];
    let msg;

    // Some tests requires mongot, however, setting this failpoint will make search queries to
    // return EOF, that way all the hassle of setting it up can be avoided.
    let disableSearchFailpoint;
    if (t.disableSearch) {
        disableSearchFailpoint = configureFailPoint(conn.rs0 ? conn.rs0.getPrimary() : conn,
                                                    'searchReturnEofImmediately');
    }
    for (let i = 0; i < t.testcases.length; i++) {
        const testcase = t.testcases[i];
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
                const p = testcase.privileges[j];
                const resource = p.resource;
                const actions = p.actions;

                // A particular privilege can explicitly specify that it should not be removed when
                // testing for authorization failure. This accommodates special-case behavior for
                // views in conjunction with the create and collMod commands.
                if (p.removeWhenTestingAuthzFailure === false) {
                    continue;
                }

                for (let k = 0; k < actions.length; k++) {
                    const privDoc = {resource: resource, actions: [actions[k]]};
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

        function specialResource(resource) {
            if (!resource) {
                return true;
            }

            // Tests which use {db: "local", collection: "oplog.rs"} will not work with
            // {db: "", collection: "oplog.rs"}. oplog.rs is special, and does not match with
            // forDatabaseName or anyNormalResource ResourcePatterns. The same is true of
            // oplog.$main, but oplog.$main is also an illegal collection name on any database
            // other than local. The other collections checked for here in the local database have
            // the same property as oplog.rs.
            return !resource.db || !resource.collection ||
                resource.collection.startsWith("system.") || resource.db == "local";
        }

        // Test for proper authorization with the test case's privileges where non-system
        // collections are modified to be the empty string.
        msg = testProperAuthorization(conn, t, testcase, testcase.privileges.map(function(priv) {
            // Make a copy of the privilege so as not to modify the original array.
            const modifiedPrivilege = Object.extend({}, priv, true);
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
            const modifiedPrivilege = Object.extend({}, priv, true);
            if (!specialResource(priv.resource)) {
                modifiedPrivilege.resource.db = "";
            }
            return modifiedPrivilege;
        }));
        if (msg) {
            failures.push(t.testname + ": " + msg);
        }
    }

    if (disableSearchFailpoint) {
        disableSearchFailpoint.off();
    }

    return failures;
}

function createUsers(conn) {
    const adminDb = conn.getDB(adminDbName);
    adminDb.createUser({user: "admin", pwd: "password", roles: ["__system"]});

    assert(adminDb.auth("admin", "password"));

    assert.commandWorked(adminDb.runCommand({createRole: testRole, privileges: [], roles: []}));
    assert.commandWorked(adminDb.runCommand(
        {createUser: testUser, pwd: "password", roles: [{role: testRole, db: adminDbName}]}));

    adminDb.logout();

    // Primary connection will now act as test user only.
    assert(adminDb.auth(testUser, "password"));
}

const opts = {
    auth: ""
};
const impls = {
    createUsers: createUsers,
    runOneTest: runOneTest
};

// run all tests standalone
{
    const conn = MongoRunner.runMongod(opts);

    // Create secondary connection to be intermittently authed
    // with admin privileges for setup/teardown.
    conn.sidechannel = new Mongo(conn.host);
    authCommandsLib.runTests(conn, impls);
    MongoRunner.stopMongod(conn);
}

// run all tests sharded
{
    const conn = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        keyFile: "jstests/libs/key1",
        other: {shardOptions: opts}
    });
    conn.sidechannel = new Mongo(conn.s0.host);
    authCommandsLib.runTests(conn, impls);
    conn.stop();
}
