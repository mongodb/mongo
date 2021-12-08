// Tests that a user can only use the $_internalReadFromClusterTime while having applyOps privilege
// when test commands are not enabled.
// @tags: [requires_replication, requires_wiredtiger, multiversion_incompatible]

(function() {
"use strict";

// Special privilege required to run applyOps command.
// Role dbAdminAnyDatabase has this privilege.
const applyOps_priv = {
    resource: {cluster: true},
    actions: ["applyOps"]
};

const testUser = "testUser";
const testUserWithDbAdminAnyDatabaseRole = "testUserWithDbAdminAnyDatabaseRole";
const testRole = "testRole";
const rsName = TestData.testName + "_rs";
const testDbName = TestData.testName;
const testCollName = "testColl";
const adminDbName = "admin";
const authErrCode = 13;
const mongoOptions = {
    auth: null,
    keyFile: "jstests/libs/key1"
};

function _getClusterTime(conn) {
    const pingRes = assert.commandWorked(conn.adminCommand({ping: 1}));
    assert(pingRes.hasOwnProperty("$clusterTime"), tojson(pingRes));
    assert(pingRes.$clusterTime.hasOwnProperty("clusterTime"), tojson(pingRes));
    return pingRes.$clusterTime.clusterTime;
}

function createUsers(conn) {
    let adminDb = conn.getDB(adminDbName);
    // Create the admin user.
    assert.commandWorked(
        adminDb.runCommand({createUser: "admin", pwd: "password", roles: ["__system"]}));

    assert(adminDb.auth("admin", "password"));
    assert.commandWorked(adminDb.runCommand({createRole: testRole, privileges: [], roles: []}));

    let testDb = adminDb.getSiblingDB(testDbName);
    assert.commandWorked(testDb.runCommand(
        {createUser: testUser, pwd: "password", roles: [{role: testRole, db: adminDbName}]}));

    assert.commandWorked(testDb.runCommand({
        createUser: testUserWithDbAdminAnyDatabaseRole,
        pwd: "password",
        roles: [{role: "dbAdminAnyDatabase", db: adminDbName}, {role: testRole, db: adminDbName}]
    }));

    adminDb.logout();
}

function testAuthorization(conn, privileges, user, shouldSucceed) {
    let testDb = conn.getDB(testDbName);
    let adminDb = conn.getDB(adminDbName);

    assert(adminDb.auth("admin", "password"));
    assert.commandWorked(adminDb.runCommand({updateRole: testRole, privileges: privileges}));
    adminDb.logout();

    assert(testDb.auth(user, "password"));

    let clusterTime = _getClusterTime(conn);
    let command = {find: testCollName, $_internalReadAtClusterTime: clusterTime};
    if (shouldSucceed) {
        assert.commandWorked(testDb.runCommand(command));
    } else {
        var res = testDb.runCommand(command);
        if (res.ok == 1 || res.code != authErrCode) {
            let msg = "expected authorization failure " +
                " but received " + tojson(res) + " with privileges " + tojson(privileges);
            assert(false, msg);
        }
    }

    testDb.logout();
}

function runTest(conn) {
    createUsers(conn);
    let adminDb = conn.getDB(adminDbName);
    let testDb = conn.getDB(testDbName);
    assert(adminDb.auth("admin", "password"));
    assert.commandWorked(testDb[testCollName].insert({_id: "Put in some data to read"}));
    adminDb.logout();

    // read and write, but not applyOps.
    let privileges = [{
        resource: {db: testDbName, collection: testCollName},
        actions: ["find", "update", "remove"]
    }];

    // Test $_internalReadAtClusterTime fails without applyOps privilege or dbAdminAnyDatabase role.
    testAuthorization(conn, privileges, testUser, false);

    // Test $_internalReadAtClusterTime succeeds with applyOps privilege.
    testAuthorization(conn, privileges.concat(applyOps_priv), testUser, true);

    // Test $_internalReadAtClusterTime succeed with dbAdminAnyDatabase role.
    testAuthorization(conn, privileges, testUserWithDbAdminAnyDatabaseRole, true);
}

// With test commands enabled, $_internalReadFromClusterTime is always available.
TestData.enableTestCommands = false;
// Parameter roleGraphInvalidationIsFatal is not compatible with disabling test commands.
TestData.roleGraphInvalidationIsFatal = false;
// Run the test on a replica set.  ReplSetTest with auth assumes test commands work so can't be
// used.
let conn = MongoRunner.runMongod({replSet: rsName, keyFile: 'jstests/libs/key1'});
conn.adminCommand({replSetInitiate: rsName});
assert.soon(() => conn.adminCommand({hello: 1}).isWritablePrimary);
runTest(conn);
MongoRunner.stopMongod(conn);
}());
