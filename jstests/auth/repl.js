// Test that authorization information gets propogated correctly to secondaries and slaves.

var baseName = "jstests_auth_repl";
var rsName = baseName + "_rs";
var rtName = baseName + "_rt";
var mongoOptions = {auth: null, keyFile: "jstests/libs/key1"};
var authErrCode = 13;

var AuthReplTest = function(spec) {
    var that = {};

    // argument validation
    assert("primaryConn" in spec);
    assert("secondaryConn" in spec);

    // private vars
    var primaryConn, secondaryConn;
    var adminPri, adminSec;
    var testUser = "testUser", testRole = "testRole", testRole2 = "testRole2";

    primaryConn = spec.primaryConn;
    secondaryConn = spec.secondaryConn;

    adminPri = primaryConn.getDB("admin");
    adminPri.createUser({user: "super", pwd: "super", roles: ["__system"]});
    assert(adminPri.auth("super", "super"), "could not authenticate as superuser");

    if (secondaryConn != null) {
        secondaryConn.setSlaveOk(true);
        adminSec = secondaryConn.getDB("admin");
    }

    /* --- private functions --- */

    var authOnSecondary = function() {
        assert(adminSec.auth(testUser, testUser), "could not authenticate as test user");
    };

    /**
     * Use the rolesInfo command to check that the test
     * role is as expected on the secondary/slave
     */
    var confirmRolesInfo = function(actionType) {
        var role = adminSec.getRole(testRole, {showPrivileges: true});
        assert.eq(1, role.privileges.length);
        assert.eq(role.privileges[0].actions[0], actionType);
    };

    /**
     * Use the usersInfo command to check that the test
     * user is as expected on the secondary/slave
     */
    var confirmUsersInfo = function(roleName) {
        var user = adminSec.getUser(testUser);
        assert.eq(1, user.roles.length);
        assert.eq(user.roles[0].role, roleName);
    };

    /**
     * Ensure that the test user has the proper privileges
     * on the secondary/slave
     */
    var confirmPrivilegeBeforeUpdate = function() {
        // can run hostInfo
        var res = adminSec.runCommand({hostInfo: 1});
        assert.commandWorked(res);

        // but cannot run listDatabases
        res = adminSec.runCommand({listDatabases: 1});
        assert.commandFailedWithCode(res, authErrCode);
    };

    var updateRole = function() {
        var res = adminPri.runCommand({
            updateRole: testRole,
            privileges: [{resource: {cluster: true}, actions: ["listDatabases"]}],
            writeConcern: {w: 2, wtimeout: 15000}
        });
        assert.commandWorked(res);
    };

    var updateUser = function() {
        var res = adminPri.runCommand(
            {updateUser: testUser, roles: [testRole2], writeConcern: {w: 2, wtimeout: 15000}});
        assert.commandWorked(res);
    };

    /**
     * Ensure that the auth changes have taken effect
     * properly on the secondary/slave
     */
    var confirmPrivilegeAfterUpdate = function() {
        // cannot run hostInfo
        var res = adminSec.runCommand({hostInfo: 1});
        assert.commandFailedWithCode(res, authErrCode);

        // but can run listDatabases
        res = adminSec.runCommand({listDatabases: 1});
        assert.commandWorked(res);
    };

    /**
     * Remove test users and roles
     */
    var cleanup = function() {
        var res = adminPri.runCommand({dropUser: testUser, writeConcern: {w: 2, wtimeout: 15000}});
        assert.commandWorked(res);
        res = adminPri.runCommand(
            {dropAllRolesFromDatabase: 1, writeConcern: {w: 2, wtimeout: 15000}});
        assert.commandWorked(res);
    };

    /* --- public functions --- */

    /**
     * Set the secondary for the test
     */
    that.setSecondary = function(secondary) {
        secondaryConn = secondary;
        secondaryConn.setSlaveOk(true);
        adminSec = secondaryConn.getDB("admin");
    };

    /**
     * Create user and roles in preparation
     * for the test.
     */
    that.createUserAndRoles = function(numNodes) {
        var roles = [testRole, testRole2];
        var actions = ["hostInfo", "listDatabases"];
        for (var i = 0; i < roles.length; i++) {
            var res = adminPri.runCommand({
                createRole: roles[i],
                privileges: [{resource: {cluster: true}, actions: [actions[i]]}],
                roles: [],
                writeConcern: {w: numNodes, wtimeout: 15000}
            });
            assert.commandWorked(res);
        }

        var res = adminPri.runCommand({
            createUser: testUser,
            pwd: testUser,
            roles: [testRole],
            writeConcern: {w: numNodes, wtimeout: 15000}
        });
        assert.commandWorked(res);
    };

    /**
     * Top-level test for updating users and roles and ensuring that the update
     * has the correct effect on the secondary/slave
     */
    that.testAll = function() {
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
var rs = new ReplSetTest({name: rsName, nodes: 2});
var nodes = rs.startSet(mongoOptions);
rs.initiate();
authutil.asCluster(nodes, "jstests/libs/key1", function() {
    rs.awaitReplication();
});

var primary = rs.getPrimary();
var secondary = rs.getSecondary();

var authReplTest = AuthReplTest({primaryConn: primary, secondaryConn: secondary});
authReplTest.createUserAndRoles(2);
authReplTest.testAll();
rs.stopSet();

jsTest.log("2 test initial sync");
rs = new ReplSetTest({name: rsName, nodes: 1, nodeOptions: mongoOptions});
nodes = rs.startSet();
rs.initiate();
authutil.asCluster(nodes, "jstests/libs/key1", function() {
    rs.awaitReplication();
});

primary = rs.getPrimary();

var authReplTest = AuthReplTest({primaryConn: primary, secondaryConn: null});
authReplTest.createUserAndRoles(1);

// Add a secondary and wait for initial sync
rs.add(mongoOptions);
rs.reInitiate();
rs.awaitSecondaryNodes();
secondary = rs.getSecondary();

authReplTest.setSecondary(secondary);
authReplTest.testAll();
rs.stopSet();

jsTest.log("3 test master/slave");
var rt = new ReplTest(rtName);

// start and stop without auth in order to ensure
// existence of the correct dbpath
var master = rt.start(true, {}, false, true);
rt.stop(true);
var slave = rt.start(false, {}, false, true);
rt.stop(false);

// start master/slave with auth
master = rt.start(true, mongoOptions, true);
slave = rt.start(false, mongoOptions, true);
var masterDB = master.getDB("admin");

masterDB.createUser({user: "root", pwd: "pass", roles: ["root"]});
masterDB.auth("root", "pass");

// ensure that master/slave replication is up and running
masterDB.foo.save({}, {writeConcern: {w: 2, wtimeout: 15000}});
masterDB.foo.drop();

authReplTest = AuthReplTest({primaryConn: master, secondaryConn: slave});
authReplTest.createUserAndRoles(2);
authReplTest.testAll();
rt.stop();
