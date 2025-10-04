/**
 * Library for testing authorization of commands with builtin roles.
 *
 * The test logic implemented here operates on the test cases defined
 * in jstests/auth/lib/commands_lib.js
 */

import {
    adminDbName,
    authCommandsLib,
    authErrCode,
    commandNotSupportedCode,
    firstDbName,
} from "jstests/auth/lib/commands_lib.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

// This test involves killing all sessions, which will not work as expected if the kill command is
// sent with an implicit session.
TestData.disableImplicitSessions = true;

export const roles = [
    {key: "read", role: "read", dbname: firstDbName},
    {key: "readLocal", role: {role: "read", db: "local"}, dbname: adminDbName},
    {key: "readAnyDatabase", role: "readAnyDatabase", dbname: adminDbName},
    {key: "readWrite", role: "readWrite", dbname: firstDbName},
    {key: "readWriteLocal", role: {role: "readWrite", db: "local"}, dbname: adminDbName},
    {key: "readWriteAnyDatabase", role: "readWriteAnyDatabase", dbname: adminDbName},
    {key: "userAdmin", role: "userAdmin", dbname: firstDbName},
    {key: "userAdminAnyDatabase", role: "userAdminAnyDatabase", dbname: adminDbName},
    {key: "dbAdmin", role: "dbAdmin", dbname: firstDbName},
    {key: "dbAdminAnyDatabase", role: "dbAdminAnyDatabase", dbname: adminDbName},
    {key: "clusterAdmin", role: "clusterAdmin", dbname: adminDbName},
    {key: "dbOwner", role: "dbOwner", dbname: firstDbName},
    {key: "enableSharding", role: "enableSharding", dbname: firstDbName},
    {key: "clusterMonitor", role: "clusterMonitor", dbname: adminDbName},
    {key: "hostManager", role: "hostManager", dbname: adminDbName},
    {key: "clusterManager", role: "clusterManager", dbname: adminDbName},
    {key: "backup", role: "backup", dbname: adminDbName},
    {key: "restore", role: "restore", dbname: adminDbName},
    {key: "root", role: "root", dbname: adminDbName},
    {key: "searchCoordinator", role: "searchCoordinator", dbname: adminDbName},
    {key: "__system", role: "__system", dbname: adminDbName},
];

/**
 * Parameters:
 *   conn -- connection, either to standalone mongod,
 *      or to mongos in sharded cluster
 *   t -- a test object from the tests array in jstests/auth/commands.js
 *   testcase -- the particular testcase from t to test
 *   r -- a role object from the "roles" array above
 *
 * Returns:
 *   An empty string on success, or an error string
 *   on test failure.
 */
function testProperAuthorization(conn, t, testcase, r) {
    let out = "";

    let authDb = conn.getDB(testcase.runOnDb);
    let state = authCommandsLib.setup(conn, t, authDb);
    assert(r.db.auth("user|" + r.key, "password"));
    authCommandsLib.authenticatedSetup(t, authDb);
    let command = t.command;
    if (typeof command === "function") {
        command = t.command(state, testcase.commandArgs);
    }
    let cmdDb = authDb;
    if (t.hasOwnProperty("runOnDb")) {
        assert.eq(typeof t.runOnDb, "function");
        cmdDb = authDb.getSiblingDB(t.runOnDb(state));
    }
    let res = cmdDb.runCommand(command);

    if (testcase.roles[r.key]) {
        if (res.ok == 0 && res.code == authErrCode) {
            out =
                "expected authorization success" +
                " but received " +
                tojson(res) +
                " on db " +
                testcase.runOnDb +
                " with role " +
                r.key;
        } else if (res.ok == 0 && !testcase.expectFail && res.code != commandNotSupportedCode) {
            // don't error if the test failed with code commandNotSupported since
            // some storage engines don't support some commands.
            out = "command failed with " + tojson(res) + " on db " + testcase.runOnDb + " with role " + r.key;
        }
    } else {
        // Don't error if the test failed with CommandNotFound rather than an authorization failure
        // because some commands may be guarded by feature flags.
        if (res.ok == 1 || (res.ok == 0 && res.code != authErrCode && res.code !== ErrorCodes.CommandNotFound)) {
            out =
                "expected authorization failure" +
                " but received result " +
                tojson(res) +
                " on db " +
                testcase.runOnDb +
                " with role " +
                r.key;
        }
    }

    r.db.logout();
    authCommandsLib.teardown(conn, t, authDb, res);
    return out;
}

/**
 * First of two entry points for this test library.
 * To be invoked as an test argument to authCommandsLib.runTests().
 */
export function runOneTest(conn, t) {
    let failures = [];

    // Some tests requires mongot, however, setting this failpoint will make search queries to
    // return EOF, that way all the hassle of setting it up can be avoided.
    let disableSearchFailpointShard, disableSearchFailpointRouter;
    if (t.disableSearch) {
        disableSearchFailpointShard = configureFailPoint(
            conn.rs0 ? conn.rs0.getPrimary() : conn,
            "searchReturnEofImmediately",
        );
        if (conn.s) {
            disableSearchFailpointRouter = configureFailPoint(conn.s, "searchReturnEofImmediately");
        }
    }

    for (let i = 0; i < t.testcases.length; i++) {
        let testcase = t.testcases[i];
        if (!("roles" in testcase)) {
            continue;
        }
        for (let j = 0; j < roles.length; j++) {
            let msg = testProperAuthorization(conn, t, testcase, roles[j]);
            if (msg) {
                failures.push(t.testname + ": " + msg);
            }
        }
    }

    if (disableSearchFailpointShard) {
        disableSearchFailpointShard.off();
    }

    if (disableSearchFailpointRouter) {
        disableSearchFailpointRouter.off();
    }
    return failures;
}

/**
 * Second entry point for this test library.
 * To be invoked as an test argument to authCommandsLib.runTests().
 */
export function createUsers(conn) {
    let adminDb = conn.getDB(adminDbName);
    adminDb.createUser({user: "admin", pwd: "password", roles: ["__system"]});

    assert(adminDb.auth("admin", "password"));
    for (let i = 0; i < roles.length; i++) {
        const r = roles[i];
        r.db = conn.getDB(r.dbname);
        r.db.createUser({user: "user|" + r.key, pwd: "password", roles: [r.role]});
    }
    adminDb.logout();
}

/**
 * This tests the authorization of commands with builtin roles for a given server configuration
 * represented in 'conn'.
 */
export function runAllCommandsBuiltinRoles(conn) {
    const testFunctionImpls = {createUsers: createUsers, runOneTest: runOneTest};
    authCommandsLib.runTests(conn, testFunctionImpls);
}
