/**
 * This tests the access control for the pseudo-commands inprog, curop, and killop.  Once
 * SERVER-5466 is resolved this test should be removed and its functionality merged into
 * commands_builtin_roles.js and commands_user_defined_roles.js.
 */

function runTest(conn) {
    var authzErrorCode = 13;

    conn.getDB('admin').createUser({user: 'admin', pwd: 'pwd', roles: ['root']});

    var adminConn = new Mongo(conn.host);
    adminConn.getDB('admin').auth('admin', 'pwd');
    var admin = adminConn.getDB('admin');
    admin.createRole({role: 'myRole', roles: [], privileges: []});
    admin.createUser({user: 'spencer', pwd: 'pwd', roles: ['myRole']});

    var db = conn.getDB('admin');
    db.auth('spencer', 'pwd');

    /**
     * Tests that a single operation has the proper authorization.  The operation is run by invoking
     * "testFunc".  "testFunc" must take a single argument indicating whether we expect the test to
     * pass or fail due to being unauthorized.  Roles in an object that described which roles are
     * expected to be able to run the operation, and privilege is the privilege that must be granted
     * a user-defined-role in order to run the operation.
     */
    function testProperAuthorization(testFunc, roles, privilege) {
        // Test built-in roles first
        for (role in roles) {
            admin.updateRole("myRole", {roles: [role]});
            testFunc(roles[role]);
        }

        // Now test user-defined role
        admin.updateRole("myRole", {roles: [], privileges: [privilege]});
        testFunc(true);

        admin.updateRole("myRole", {roles: [], privileges: []});
        testFunc(false);
    }

    /**
     * Returns true if conn is a connection to mongos,
     * and false otherwise.
     */
    function isMongos(db) {
        var res = db.adminCommand({isdbgrid: 1});
        return (res.ok == 1 && res.isdbgrid == 1);
    }

    (function testInprog() {
        jsTestLog("Testing inprog");

        var roles = {
            read: false,
            readAnyDatabase: false,
            readWrite: false,
            readWriteAnyDatabase: false,
            dbAdmin: false,
            dbAdminAnyDatabase: false,
            dbOwner: false,
            clusterMonitor: true,
            clusterManager: false,
            hostManager: false,
            clusterAdmin: true,
            root: true,
            __system: true
        };

        var privilege = {resource: {cluster: true}, actions: ['inprog']};

        var testFunc = function(shouldPass) {
            var passed = true;
            try {
                var res = db.currentOp();
                passed = res.ok && !res.hasOwnProperty("errmsg");
            } catch (e) {
                passed = false;
            }

            assert.eq(shouldPass, passed);
            if (shouldPass) {
                assert.gte(res.inprog.length, 0);
            }
        };

        testProperAuthorization(testFunc, roles, privilege);
    })();

    (function testKillop() {
        jsTestLog("Testing killOp");

        var roles = {
            read: false,
            readAnyDatabase: false,
            readWrite: false,
            readWriteAnyDatabase: false,
            dbAdmin: false,
            dbAdminAnyDatabase: false,
            dbOwner: false,
            clusterMonitor: false,
            clusterManager: false,
            hostManager: true,
            clusterAdmin: true,
            root: true,
            __system: true
        };

        var privilege = {resource: {cluster: true}, actions: ['killop']};

        var testFunc = function(shouldPass) {
            var passed = true;
            try {
                var opid;
                if (isMongos(db)) {  // opid format different between mongos and mongod
                    opid = "shard0000:1234";
                } else {
                    opid = 1234;
                }
                var res = db.killOp(opid);
                printjson(res);
                passed = res.ok && !res.errmsg && !res.err && !res['$err'];
            } catch (e) {
                passed = false;
            }
            assert.eq(shouldPass, passed);
        };

        testProperAuthorization(testFunc, roles, privilege);
    })();

    (function testUnlock() {
        if (isMongos(db)) {
            return;  // unlock doesn't work on mongos
        }

        jsTestLog("Testing unlock");

        var roles = {
            read: false,
            readAnyDatabase: false,
            readWrite: false,
            readWriteAnyDatabase: false,
            dbAdmin: false,
            dbAdminAnyDatabase: false,
            dbOwner: false,
            clusterMonitor: false,
            clusterManager: false,
            hostManager: true,
            clusterAdmin: true,
            root: true,
            __system: true
        };

        var privilege = {resource: {cluster: true}, actions: ['unlock']};

        var testFunc = function(shouldPass) {
            var passed = true;
            try {
                var ret = admin.fsyncLock();  // must be locked first
                // If the storage engine doesnt support fsync lock, we can't proceed
                if (!ret.ok) {
                    assert.commandFailedWithCode(ret, ErrorCodes.CommandNotSupported);
                    assert(
                        shouldPass);  // If we get to the storage engine, we better be authorized.
                    return;
                }
                var res = db.fsyncUnlock();
                printjson(res);
                passed = res.ok && !res.errmsg && !res.err && !res['$err'];
                passed = passed || false;  // convert undefined to false
            } catch (e) {
                passed = false;
            }
            if (!passed) {
                admin.fsyncUnlock();
            }

            assert.eq(shouldPass, passed);
        };

        testProperAuthorization(testFunc, roles, privilege);
    })();
}

jsTest.log('Test standalone');
var conn = MongoRunner.runMongod({auth: ''});
runTest(conn);
MongoRunner.stopMongod(conn.port);

jsTest.log('Test sharding');
var st = new ShardingTest({shards: 2, config: 3, keyFile: 'jstests/libs/key1'});
runTest(st.s);
st.stop();
