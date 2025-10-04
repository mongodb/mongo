/**
 * This file tests basic authorization for different roles in stand alone and sharded
 * environment. This file covers all types of operations except commands.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

/**
 * Data structure that contains all the users that are going to be used in the tests.
 * The structure is as follows:
 * 1st level field name: database names.
 * 2nd level field name: user names.
 * 3rd level is an object that has the format:
 *     { pwd: <password>, roles: [<list of roles>] }
 * @tags: [requires_sharding]
 */
let AUTH_INFO = {
    admin: {
        root: {pwd: "root", roles: ["root"]},
        cluster: {pwd: "cluster", roles: ["clusterAdmin"]},
        anone: {pwd: "none", roles: []},
        aro: {pwd: "ro", roles: ["read"]},
        arw: {pwd: "rw", roles: ["readWrite"]},
        aadmin: {pwd: "admin", roles: ["dbAdmin"]},
        auadmin: {pwd: "uadmin", roles: ["userAdmin"]},
        any_ro: {pwd: "ro", roles: ["readAnyDatabase"]},
        any_rw: {pwd: "rw", roles: ["readWriteAnyDatabase"]},
        any_admin: {pwd: "admin", roles: ["dbAdminAnyDatabase"]},
        any_uadmin: {pwd: "uadmin", roles: ["userAdminAnyDatabase"]},
    },
    test: {
        none: {pwd: "none", roles: []},
        ro: {pwd: "ro", roles: ["read"]},
        rw: {pwd: "rw", roles: ["readWrite"]},
        roadmin: {pwd: "roadmin", roles: ["read", "dbAdmin"]},
        admin: {pwd: "admin", roles: ["dbAdmin"]},
        uadmin: {pwd: "uadmin", roles: ["userAdmin"]},
    },
};

// Constants that lists the privileges of a given role.
let READ_PERM = {query: 1, index_r: 1, killCursor: 1};
let READ_WRITE_PERM = {insert: 1, update: 1, remove: 1, query: 1, index_r: 1, index_w: 1, killCursor: 1};
let ADMIN_PERM = {index_r: 1, index_w: 1, profile_r: 1};
let UADMIN_PERM = {user_r: 1, user_w: 1};
let CLUSTER_PERM = {killOp: 1, currentOp: 1, fsync_unlock: 1, killCursor: 1, killAnyCursor: 1, profile_r: 1};

/**
 * Checks whether an error occurs after running an operation.
 *
 * @param shouldPass {Boolean} true means that the operation should succeed.
 * @param opFunc {function()} a function object which contains the operation to perform.
 */
let checkErr = function (shouldPass, opFunc) {
    let success = true;

    let exception = null;
    try {
        opFunc();
    } catch (x) {
        exception = x;
        success = false;
    }

    assert(
        success == shouldPass,
        "expected shouldPass: " +
            shouldPass +
            ", got: " +
            success +
            ", op: " +
            tojson(opFunc) +
            ", exception: " +
            tojson(exception),
    );
};

/**
 * Runs a series of operations against the db provided.
 *
 * @param db {DB} the database object to use.
 * @param allowedActions {Object} the lists of operations that are allowed for the
 *     current user. The data structure is represented as a map with the presence of
 *     a field name means that the operation is allowed and not allowed if it is
 *     not present. The list of field names are: insert, update, remove, query, killOp,
 *     currentOp, index_r, index_w, profile_r, profile_w, user_r, user_w, killCursor,
 *     fsync_unlock.
 */
let testOps = function (db, allowedActions) {
    checkErr(allowedActions.hasOwnProperty("insert"), function () {
        let res = db.user.insert({y: 1});
        if (res.hasWriteError()) throw Error("insert failed: " + tojson(res.getRawResponse()));
    });

    checkErr(allowedActions.hasOwnProperty("update"), function () {
        let res = db.user.update({y: 1}, {z: 3});
        if (res.hasWriteError()) throw Error("update failed: " + tojson(res.getRawResponse()));
    });

    checkErr(allowedActions.hasOwnProperty("remove"), function () {
        let res = db.user.remove({y: 1});
        if (res.hasWriteError()) throw Error("remove failed: " + tojson(res.getRawResponse()));
    });

    checkErr(allowedActions.hasOwnProperty("query"), function () {
        db.user.findOne({y: 1});
    });

    checkErr(allowedActions.hasOwnProperty("killOp"), function () {
        let errorCodeUnauthorized = 13;
        let res = db.killOp(1);

        if (res.code == errorCodeUnauthorized) {
            throw Error("unauthorized killOp");
        }
    });

    checkErr(allowedActions.hasOwnProperty("currentOp"), function () {
        let errorCodeUnauthorized = 13;
        let res = db.currentOp();

        if (res.code == errorCodeUnauthorized) {
            throw Error("unauthorized currentOp");
        }
    });

    checkErr(allowedActions.hasOwnProperty("index_r"), function () {
        let errorCodeUnauthorized = 13;
        let res = db.runCommand({"listIndexes": "user"});

        if (res.code == errorCodeUnauthorized) {
            throw Error("unauthorized listIndexes");
        }
    });

    checkErr(allowedActions.hasOwnProperty("index_w"), function () {
        let res = db.user.createIndex({x: 1});
        if (res.code == 13) {
            // Unauthorized
            throw Error("unauthorized currentOp");
        }
    });

    checkErr(allowedActions.hasOwnProperty("profile_r"), function () {
        db.system.profile.findOne();
    });

    checkErr(allowedActions.hasOwnProperty("profile_w"), function () {
        let res = db.system.profile.insert({x: 1});
        if (res.hasWriteError()) {
            throw Error("profile insert failed: " + tojson(res.getRawResponse()));
        }
    });

    checkErr(allowedActions.hasOwnProperty("user_r"), function () {
        let result = db.runCommand({usersInfo: 1});
        if (!result.ok) {
            throw new Error(tojson(result));
        }
    });

    checkErr(allowedActions.hasOwnProperty("user_w"), function () {
        db.createUser({user: "a", pwd: "a", roles: jsTest.basicUserRoles});
        assert(db.dropUser("a"));
    });

    // Test for kill cursor
    const checkKillCursor = function (inTransaction) {
        let newConn = new Mongo(db.getMongo().host);
        let dbName = db.getName();
        let db2 = newConn.getDB(dbName);

        if (db2.getName() == "admin") {
            assert.eq(1, db2.auth("aro", AUTH_INFO.admin.aro.pwd));
        } else {
            assert.eq(1, db2.auth("ro", AUTH_INFO.test.ro.pwd));
        }

        // Create cursor from db2.
        let cmdRes = db2.runCommand({
            find: db2.kill_cursor.getName(),
            batchSize: 2,
            ...(inTransaction ? {startTransaction: true, autocommit: false, txnNumber: NumberLong(0)} : {}),
        });
        assert.commandWorked(cmdRes);
        let cursorId = cmdRes.cursor.id;
        assert(
            !bsonBinaryEqual({cursorId: cursorId}, {cursorId: NumberLong(0)}),
            "find command didn't return a cursor: " + tojson(cmdRes),
        );

        const shouldSucceed = (function () {
            // admin users can do anything they want.
            if (allowedActions.hasOwnProperty("killAnyCursor")) {
                return true;
            }

            // users can kill their own cursors
            const users = assert.commandWorked(db.runCommand({connectionStatus: 1})).authInfo.authenticatedUsers;
            const users2 = assert.commandWorked(db2.runCommand({connectionStatus: 1})).authInfo.authenticatedUsers;
            if (!users.length && !users2.length) {
                // Special case, no-auth
                return true;
            }
            return users.some(function (u) {
                return users2.some(function (u2) {
                    return u.db === u2.db && u.user === u2.user;
                });
            });
        })();

        checkErr(shouldSucceed, function () {
            // Issue killCursor command from db.
            cmdRes = db.runCommand({killCursors: db2.kill_cursor.getName(), cursors: [cursorId]});
            assert.commandWorked(cmdRes);
            assert(
                bsonBinaryEqual({cursorId: cmdRes.cursorsKilled}, {cursorId: [cursorId]}),
                "unauthorized to kill cursor: " + tojson(cmdRes),
            );
        });
        if (inTransaction) {
            assert.commandWorked(db2.adminCommand({abortTransaction: 1, txnNumber: NumberLong(0), autocommit: false}));
        }
    };
    checkKillCursor(false);

    let isMongos = db.runCommand({isdbgrid: 1}).isdbgrid;
    // Note: fsyncUnlock is not supported in mongos.
    if (!isMongos) {
        checkErr(allowedActions.hasOwnProperty("fsync_unlock"), function () {
            let res = db.fsyncUnlock();
            let errorCodeUnauthorized = 13;

            if (res.code == errorCodeUnauthorized) {
                throw Error("unauthorized fsyncUnlock");
            }
        });
    } else {
        // Requires transactions and the non-sharded version is on standalone
        checkKillCursor(true);
    }
};

// List of tests to run. Has the format:
//
// {
//   name: {String} description of the test
//   test: {function(Mongo)} the test function to run which accepts a Mongo connection
//                           object.
// }
let TESTS = [
    {
        name: "Test multiple user login separate connection",
        test: function (conn) {
            let testDB = conn.getDB("test");
            assert.eq(1, testDB.auth("ro", AUTH_INFO.test.ro.pwd));

            let conn2 = new Mongo(conn.host);
            let testDB2 = conn2.getDB("test");
            assert.eq(1, testDB2.auth("uadmin", AUTH_INFO.test.uadmin.pwd));

            testOps(testDB, READ_PERM);
            testOps(testDB2, UADMIN_PERM);
        },
    },
    {
        name: "Test user with no role",
        test: function (conn) {
            let testDB = conn.getDB("test");
            assert.eq(1, testDB.auth("none", AUTH_INFO.test.none.pwd));

            testOps(testDB, {});
        },
    },
    {
        name: "Test read only user",
        test: function (conn) {
            let testDB = conn.getDB("test");
            assert.eq(1, testDB.auth("ro", AUTH_INFO.test.ro.pwd));

            testOps(testDB, READ_PERM);
        },
    },
    {
        name: "Test read/write user",
        test: function (conn) {
            let testDB = conn.getDB("test");
            assert.eq(1, testDB.auth("rw", AUTH_INFO.test.rw.pwd));

            testOps(testDB, READ_WRITE_PERM);
        },
    },
    {
        name: "Test read + dbAdmin user",
        test: function (conn) {
            let testDB = conn.getDB("test");
            assert.eq(1, testDB.auth("roadmin", AUTH_INFO.test.roadmin.pwd));

            let combinedPerm = Object.extend({}, READ_PERM);
            combinedPerm = Object.extend(combinedPerm, ADMIN_PERM);
            testOps(testDB, combinedPerm);
        },
    },
    {
        name: "Test dbAdmin user",
        test: function (conn) {
            let testDB = conn.getDB("test");
            assert.eq(1, testDB.auth("admin", AUTH_INFO.test.admin.pwd));

            testOps(testDB, ADMIN_PERM);
        },
    },
    {
        name: "Test userAdmin user",
        test: function (conn) {
            let testDB = conn.getDB("test");
            assert.eq(1, testDB.auth("uadmin", AUTH_INFO.test.uadmin.pwd));

            testOps(testDB, UADMIN_PERM);
        },
    },
    {
        name: "Test cluster user",
        test: function (conn) {
            let adminDB = conn.getDB("admin");
            assert.eq(1, adminDB.auth("cluster", AUTH_INFO.admin.cluster.pwd));

            testOps(conn.getDB("test"), CLUSTER_PERM);
        },
    },
    {
        name: "Test admin user with no role",
        test: function (conn) {
            let adminDB = conn.getDB("admin");
            assert.eq(1, adminDB.auth("anone", AUTH_INFO.admin.anone.pwd));

            testOps(adminDB, {});
            testOps(conn.getDB("test"), {});
        },
    },
    {
        name: "Test read only admin user",
        test: function (conn) {
            let adminDB = conn.getDB("admin");
            assert.eq(1, adminDB.auth("aro", AUTH_INFO.admin.aro.pwd));

            testOps(adminDB, READ_PERM);
            testOps(conn.getDB("test"), {});
        },
    },
    {
        name: "Test read/write admin user",
        test: function (conn) {
            let adminDB = conn.getDB("admin");
            assert.eq(1, adminDB.auth("arw", AUTH_INFO.admin.arw.pwd));

            testOps(adminDB, READ_WRITE_PERM);
            testOps(conn.getDB("test"), {});
        },
    },
    {
        name: "Test dbAdmin admin user",
        test: function (conn) {
            let adminDB = conn.getDB("admin");
            assert.eq(1, adminDB.auth("aadmin", AUTH_INFO.admin.aadmin.pwd));

            testOps(adminDB, ADMIN_PERM);
            testOps(conn.getDB("test"), {});
        },
    },
    {
        name: "Test userAdmin admin user",
        test: function (conn) {
            let adminDB = conn.getDB("admin");
            assert.eq(1, adminDB.auth("auadmin", AUTH_INFO.admin.auadmin.pwd));

            testOps(adminDB, UADMIN_PERM);
            testOps(conn.getDB("test"), {});
        },
    },
    {
        name: "Test read only any db user",
        test: function (conn) {
            let adminDB = conn.getDB("admin");
            assert.eq(1, adminDB.auth("any_ro", AUTH_INFO.admin.any_ro.pwd));

            testOps(adminDB, READ_PERM);
            testOps(conn.getDB("test"), READ_PERM);
        },
    },
    {
        name: "Test read/write any db user",
        test: function (conn) {
            let adminDB = conn.getDB("admin");
            assert.eq(1, adminDB.auth("any_rw", AUTH_INFO.admin.any_rw.pwd));

            testOps(adminDB, READ_WRITE_PERM);
            testOps(conn.getDB("test"), READ_WRITE_PERM);
        },
    },
    {
        name: "Test dbAdmin any db user",
        test: function (conn) {
            let adminDB = conn.getDB("admin");
            assert.eq(1, adminDB.auth("any_admin", AUTH_INFO.admin.any_admin.pwd));

            testOps(adminDB, ADMIN_PERM);
            testOps(conn.getDB("test"), ADMIN_PERM);
        },
    },
    {
        name: "Test userAdmin any db user",
        test: function (conn) {
            let adminDB = conn.getDB("admin");
            assert.eq(1, adminDB.auth("any_uadmin", AUTH_INFO.admin.any_uadmin.pwd));

            testOps(adminDB, UADMIN_PERM);
            testOps(conn.getDB("test"), UADMIN_PERM);
        },
    },

    {
        name: "Test change role",
        test: function (conn) {
            let testDB = conn.getDB("test");
            assert.eq(1, testDB.auth("rw", AUTH_INFO.test.rw.pwd));

            let newConn = new Mongo(conn.host);
            assert.eq(1, newConn.getDB("admin").auth("any_uadmin", AUTH_INFO.admin.any_uadmin.pwd));
            newConn.getDB("test").updateUser("rw", {roles: ["read"]});
            let origSpec = newConn.getDB("test").getUser("rw");

            // role change should affect users already authenticated.
            testOps(testDB, READ_PERM);

            // role change should affect active connections.
            testDB.runCommand({logout: 1});
            assert.eq(1, testDB.auth("rw", AUTH_INFO.test.rw.pwd));
            testOps(testDB, READ_PERM);

            // role change should also affect new connections.
            let newConn3 = new Mongo(conn.host);
            let testDB3 = newConn3.getDB("test");
            assert.eq(1, testDB3.auth("rw", AUTH_INFO.test.rw.pwd));
            testOps(testDB3, READ_PERM);

            newConn.getDB("test").updateUser("rw", {roles: origSpec.roles});
        },
    },

    {
        name: "Test override user",
        test: function (conn) {
            let testDB = conn.getDB("test");
            assert.eq(1, testDB.auth("rw", AUTH_INFO.test.rw.pwd));
            testDB.logout();
            assert.eq(1, testDB.auth("ro", AUTH_INFO.test.ro.pwd));
            testOps(testDB, READ_PERM);

            testDB.runCommand({logout: 1});
            testOps(testDB, {});
        },
    },
];

/**
 * Driver method for setting up the test environment, running them, cleanup
 * after every test and keeping track of test failures.
 *
 * @param conn {Mongo} a connection to a mongod or mongos to test.
 */
let runTests = function (conn) {
    let setup = function () {
        let testDB = conn.getDB("test");
        let adminDB = conn.getDB("admin");

        adminDB.createUser({user: "root", pwd: AUTH_INFO.admin.root.pwd, roles: AUTH_INFO.admin.root.roles});
        adminDB.auth("root", AUTH_INFO.admin.root.pwd);

        for (let x = 0; x < 10; x++) {
            testDB.kill_cursor.insert({x: x});
            adminDB.kill_cursor.insert({x: x});
        }

        for (let dbName in AUTH_INFO) {
            let dbObj = AUTH_INFO[dbName];

            for (let userName in dbObj) {
                if (dbName == "admin" && userName == "root") {
                    // We already registered this user.
                    continue;
                }

                let info = dbObj[userName];
                conn.getDB(dbName).createUser({user: userName, pwd: info.pwd, roles: info.roles});
            }
        }

        adminDB.runCommand({logout: 1});
    };

    let teardown = function () {
        let adminDB = conn.getDB("admin");
        adminDB.auth("root", AUTH_INFO.admin.root.pwd);
        conn.getDB("test").dropDatabase();
        adminDB.dropDatabase();
    };

    let failures = [];
    setup();
    TESTS.forEach(function (testFunc) {
        try {
            jsTest.log(testFunc.name);

            let newConn = new Mongo(conn.host);
            newConn.host = conn.host;
            testFunc.test(newConn);
        } catch (x) {
            failures.push(testFunc.name);
            jsTestLog(tojson(x));
        }
    });

    teardown();

    if (failures.length > 0) {
        let list = "";
        failures.forEach(function (test) {
            list += test + "\n";
        });
        throw Error("Tests failed:\n" + list);
    }
};

let conn = MongoRunner.runMongod({auth: ""});
runTests(conn);
MongoRunner.stopMongod(conn);

jsTest.log("Test sharding");
let st = new ShardingTest({shards: 1, keyFile: "jstests/libs/key1"});
runTests(st.s);
st.stop();

print("SUCCESS! Completed basic_role_auth.js");
