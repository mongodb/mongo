// Test that authorization information gets propogated correctly to secondaries.

import {ReplSetTest} from "jstests/libs/replsettest.js";

let baseName = "jstests_auth_repl";
let rsName = baseName + "_rs";
let mongoOptions = {auth: null, keyFile: "jstests/libs/key1"};
let authErrCode = 13;

let AuthReplTest = function (spec) {
    let that = {};

    // argument validation
    assert("primaryConn" in spec);
    assert("secondaryConn" in spec);

    // private vars
    let primaryConn, secondaryConn;
    let adminPri, adminSec;
    let testUser = "testUser",
        testRole = "testRole",
        testRole2 = "testRole2";

    primaryConn = spec.primaryConn;
    secondaryConn = spec.secondaryConn;

    adminPri = primaryConn.getDB("admin");
    adminPri.createUser({user: "super", pwd: "super", roles: ["__system"]});
    assert(adminPri.auth("super", "super"), "could not authenticate as superuser");

    if (secondaryConn != null) {
        secondaryConn.setSecondaryOk();
        adminSec = secondaryConn.getDB("admin");
    }

    /* --- private functions --- */

    let authOnSecondary = function () {
        assert(adminSec.auth(testUser, testUser), "could not authenticate as test user");
    };

    /**
     * Use the rolesInfo command to check that the test
     * role is as expected on the secondary
     */
    let confirmRolesInfo = function (actionType) {
        let role = adminSec.getRole(testRole, {showPrivileges: true});
        assert.eq(1, role.privileges.length);
        assert.eq(role.privileges[0].actions[0], actionType);
    };

    /**
     * Use the usersInfo command to check that the test
     * user is as expected on the secondary
     */
    let confirmUsersInfo = function (roleName) {
        let user = adminSec.getUser(testUser);
        assert.eq(1, user.roles.length);
        assert.eq(user.roles[0].role, roleName);
    };

    /**
     * Ensure that the test user has the proper privileges
     * on the secondary
     */
    let confirmPrivilegeBeforeUpdate = function () {
        // can run hostInfo
        let res = adminSec.runCommand({hostInfo: 1});
        assert.commandWorked(res);

        // but cannot run listDatabases
        res = adminSec.runCommand({listDatabases: 1});
        assert.commandFailedWithCode(res, authErrCode);
    };

    let updateRole = function () {
        let res = adminPri.runCommand({
            updateRole: testRole,
            privileges: [{resource: {cluster: true}, actions: ["listDatabases"]}],
            writeConcern: {w: 2, wtimeout: 15000},
        });
        assert.commandWorked(res);
    };

    let updateUser = function () {
        let res = adminPri.runCommand({
            updateUser: testUser,
            roles: [testRole2],
            writeConcern: {w: 2, wtimeout: 15000},
        });
        assert.commandWorked(res);
    };

    /**
     * Ensure that the auth changes have taken effect
     * properly on the secondary
     */
    let confirmPrivilegeAfterUpdate = function () {
        // cannot run hostInfo
        let res = adminSec.runCommand({hostInfo: 1});
        assert.commandFailedWithCode(res, authErrCode);

        // but can run listDatabases
        res = adminSec.runCommand({listDatabases: 1});
        assert.commandWorked(res);
    };

    /**
     * Remove test users and roles
     */
    let cleanup = function () {
        let res = adminPri.runCommand({dropUser: testUser, writeConcern: {w: 2, wtimeout: 15000}});
        assert.commandWorked(res);
        res = adminPri.runCommand({dropAllRolesFromDatabase: 1, writeConcern: {w: 2, wtimeout: 15000}});
        assert.commandWorked(res);
    };

    /* --- public functions --- */

    /**
     * Set the secondary for the test
     */
    that.setSecondary = function (secondary) {
        secondaryConn = secondary;
        secondaryConn.setSecondaryOk();
        adminSec = secondaryConn.getDB("admin");
    };

    /**
     * Create user and roles in preparation
     * for the test.
     */
    that.createUserAndRoles = function (numNodes) {
        let roles = [testRole, testRole2];
        let actions = ["hostInfo", "listDatabases"];
        for (let i = 0; i < roles.length; i++) {
            var res = adminPri.runCommand({
                createRole: roles[i],
                privileges: [{resource: {cluster: true}, actions: [actions[i]]}],
                roles: [],
                writeConcern: {w: numNodes, wtimeout: 15000},
            });
            assert.commandWorked(res);
        }

        res = adminPri.runCommand({
            createUser: testUser,
            pwd: testUser,
            roles: [testRole],
            writeConcern: {w: numNodes, wtimeout: 15000},
        });
        assert.commandWorked(res);
    };

    /**
     * Top-level test for updating users and roles and ensuring that the update
     * has the correct effect on the secondary
     */
    that.testAll = function () {
        authOnSecondary();
        confirmPrivilegeBeforeUpdate();
        confirmUsersInfo(testRole);
        confirmRolesInfo("hostInfo");

        updateRole();
        confirmPrivilegeAfterUpdate();
        confirmRolesInfo("listDatabases");

        updateUser();
        confirmPrivilegeAfterUpdate();
        confirmUsersInfo(testRole2);

        cleanup();
    };

    return that;
};

jsTest.log("1 test replica sets");
let rs = new ReplSetTest({name: rsName, nodes: 2});
let nodes = rs.startSet(mongoOptions);
rs.initiate();
authutil.asCluster(nodes, "jstests/libs/key1", function () {
    rs.awaitReplication();
});

let primary = rs.getPrimary();
let secondary = rs.getSecondary();

let authReplTest = AuthReplTest({primaryConn: primary, secondaryConn: secondary});
authReplTest.createUserAndRoles(2);
authReplTest.testAll();
rs.stopSet();

jsTest.log("2 test initial sync");
rs = new ReplSetTest({name: rsName, nodes: 1, nodeOptions: mongoOptions});
nodes = rs.startSet();
rs.initiate();
authutil.asCluster(nodes, "jstests/libs/key1", function () {
    rs.awaitReplication();
});

primary = rs.getPrimary();

authReplTest = AuthReplTest({primaryConn: primary, secondaryConn: null});
authReplTest.createUserAndRoles(1);

// Add a secondary and wait for initial sync
rs.add(mongoOptions);
rs.reInitiate();
rs.awaitSecondaryNodes();
secondary = rs.getSecondary();

authReplTest.setSecondary(secondary);
authReplTest.testAll();
rs.stopSet();
