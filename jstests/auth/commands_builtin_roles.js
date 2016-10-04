/*

Exhaustive test for authorization of commands with builtin roles.

The test logic implemented here operates on the test cases defined
in jstests/auth/commands.js.

*/

load("jstests/auth/lib/commands_lib.js");

var roles = [
    {key: "read", role: "read", dbname: firstDbName},
    {key: "readAnyDatabase", role: "readAnyDatabase", dbname: adminDbName},
    {key: "readWrite", role: "readWrite", dbname: firstDbName},
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
    {key: "__system", role: "__system", dbname: adminDbName}
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
    var out = "";

    var runOnDb = conn.getDB(testcase.runOnDb);
    authCommandsLib.setup(conn, t, runOnDb);
    assert(r.db.auth("user|" + r.role, "password"));
    var res = runOnDb.runCommand(t.command);

    if (testcase.roles[r.role]) {
        if (res.ok == 0 && res.code == authErrCode) {
            out = "expected authorization success" + " but received " + tojson(res) + " on db " +
                testcase.runOnDb + " with role " + r.key;
        } else if (res.ok == 0 && !testcase.expectFail && res.code != commandNotSupportedCode) {
            // don't error if the test failed with code commandNotSupported since
            // some storage engines (e.g wiredTiger) don't support some commands (e.g. touch)
            out = "command failed with " + tojson(res) + " on db " + testcase.runOnDb +
                " with role " + r.key;
        }
        // test can provide a function that will run if
        // the command completed successfully
        else if (testcase.onSuccess) {
            testcase.onSuccess(res);
        }
    } else {
        if (res.ok == 1 || (res.ok == 0 && res.code != authErrCode)) {
            out = "expected authorization failure" + " but received result " + tojson(res) +
                " on db " + testcase.runOnDb + " with role " + r.key;
        }
    }

    r.db.logout();
    authCommandsLib.teardown(conn, t, runOnDb);
    return out;
}

function runOneTest(conn, t) {
    var failures = [];

    for (var i = 0; i < t.testcases.length; i++) {
        var testcase = t.testcases[i];
        if (!("roles" in testcase)) {
            continue;
        }
        for (var j = 0; j < roles.length; j++) {
            var msg = testProperAuthorization(conn, t, testcase, roles[j]);
            if (msg) {
                failures.push(t.testname + ": " + msg);
            }
        }
    }

    return failures;
}

function createUsers(conn) {
    var adminDb = conn.getDB(adminDbName);
    adminDb.createUser({user: "admin", pwd: "password", roles: ["__system"]});

    assert(adminDb.auth("admin", "password"));
    for (var i = 0; i < roles.length; i++) {
        r = roles[i];
        r.db = conn.getDB(r.dbname);
        r.db.createUser({user: "user|" + r.role, pwd: "password", roles: [r.role]});
    }
    adminDb.logout();
}

/*
 * Makes sure that none of the test cases reference roles
 * that aren't part of the global "roles" array.
 */
function checkForNonExistentRoles() {
    var tests = authCommandsLib.tests;
    for (var i = 0; i < tests.length; i++) {
        var test = tests[i];
        for (var j = 0; j < test.testcases.length; j++) {
            var testcase = test.testcases[j];
            for (role in testcase.roles) {
                var roleExists = false;
                for (var k = 0; k < roles.length; k++) {
                    if (roles[k].role === role) {
                        roleExists = true;
                        break;
                    }
                }
                assert(roleExists,
                       "Role " + role + " found in test: " + test.testname +
                           ", but doesn't exist in roles array");
            }
        }
    }
}

var opts = {auth: "", enableExperimentalStorageDetailsCmd: ""};
var impls = {createUsers: createUsers, runOneTest: runOneTest};

checkForNonExistentRoles();

// run all tests standalone
var conn = MongoRunner.runMongod(opts);
authCommandsLib.runTests(conn, impls);
MongoRunner.stopMongod(conn);

// run all tests sharded
conn = new ShardingTest(
    {shards: 2, mongos: 1, keyFile: "jstests/libs/key1", other: {shardOptions: opts}});
authCommandsLib.runTests(conn, impls);
conn.stop();
