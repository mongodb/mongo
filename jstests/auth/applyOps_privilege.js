// Tests that a user can only run a applyops while having applyOps privilege.
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
const testDBName = "test_applyOps_auth";
const adminDbName = "admin";
const authErrCode = 13;

const command = {
    applyOps: [{
        "op": "c",
        "ns": testDBName + ".$cmd",
        "o": {
            "create": "x",
        }
    }]
};

function createUsers(conn) {
    let adminDb = conn.getDB(adminDbName);
    // Create the admin user.
    assert.commandWorked(
        adminDb.runCommand({createUser: "admin", pwd: "password", roles: ["__system"]}));

    assert(adminDb.auth("admin", "password"));
    assert.commandWorked(adminDb.runCommand({createRole: testRole, privileges: [], roles: []}));

    let testDb = adminDb.getSiblingDB(testDBName);
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
    let testDb = conn.getDB(testDBName);
    let adminDb = conn.getDB(adminDbName);

    assert(adminDb.auth("admin", "password"));
    assert.commandWorked(adminDb.runCommand({updateRole: testRole, privileges: privileges}));
    adminDb.logout();

    assert(testDb.auth(user, "password"));
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
    let privileges = [{resource: {db: testDBName, collection: "x"}, actions: ["createCollection"]}];

    // Test applyOps failed without applyOps privilege or dbAdminAnyDatabase role.
    testAuthorization(conn, privileges, testUser, false);

    // Test applyOps succeed with applyOps privilege.
    testAuthorization(conn, privileges.concat(applyOps_priv), testUser, true);

    // Test applyOps succeed with dbAdminAnyDatabase role.
    testAuthorization(conn, privileges, testUserWithDbAdminAnyDatabaseRole, true);
}

// Run the test on a standalone.
let conn = MongoRunner.runMongod({auth: ""});
runTest(conn);
MongoRunner.stopMongod(conn);
}());
