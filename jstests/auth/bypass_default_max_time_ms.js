/**
 * Tests that 'defaultMaxTimeMS' is correctly bypassed when the 'bypassDefaultMaxTimeMS' privilege
 * is granted.
 *
 * @tags: [
 *   creates_and_authenticates_user,
 *   requires_fcv_80,
 *   # Transactions aborted upon fcv upgrade or downgrade; cluster parameters use internal txns.
 *   requires_auth,
 *   requires_replication,
 *   requires_sharding,
 *   # Uses $function
 *   requires_scripting,
 *   uses_transactions,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function setDefaultReadMaxTimeMS(db, newValue) {
    assert.commandWorked(
        db.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: newValue}}}),
    );

    // Currently, the mongos cluster parameter cache is not updated on setClusterParameter. An
    // explicit call to getClusterParameter will refresh the cache.
    assert.commandWorked(db.runCommand({getClusterParameter: "defaultMaxTimeMS"}));
}

function setup(conn, getConn) {
    // Create a global admin user.
    {
        const adminDB = conn.getDB("admin");
        adminDB.createUser({user: "admin", pwd: "admin", roles: ["root"]});
    }

    const adminDB = getConn("admin", "admin", "admin");

    // Prepare a regular user without the 'bypassDefaultMaxtimeMS' privilege.
    adminDB.createUser({user: "regularUser", pwd: "password", roles: ["readAnyDatabase"]});

    // Prepare a user with the 'bypassDefaultMaxtimeMS' privilege.
    adminDB.createRole({
        role: "bypassDefaultMaxtimeMSRole",
        privileges: [{resource: {cluster: true}, actions: ["bypassDefaultMaxTimeMS"]}],
        roles: [],
    });

    adminDB.createUser({
        user: "bypassUser",
        pwd: "password",
        roles: ["readAnyDatabase", "bypassDefaultMaxtimeMSRole"],
    });

    const dbName = jsTestName();
    const testDB = adminDB.getSiblingDB(dbName);
    const collName = "test";
    const coll = testDB.getCollection(collName);

    // Insert some data to be queried
    for (let i = 0; i < 10; ++i) {
        assert.commandWorked(coll.insert({a: 1}));
    }

    const slowStage = {
        $match: {
            $expr: {
                $function: {
                    body: function () {
                        sleep(1000);
                        return true;
                    },
                    args: [],
                    lang: "js",
                },
            },
        },
    };

    return {
        aggregate: collName,
        pipeline: [slowStage],
        cursor: {},
    };
}

function runBypassTests(getConn, commandToRun, dbName = jsTestName()) {
    const adminDB = getConn("admin", "admin", "admin");
    // Sets the default maxTimeMS for read operations with a small value.
    setDefaultReadMaxTimeMS(adminDB, 1);

    // Expect failure for the regular user.
    const regularUserDB = getConn(dbName, "regularUser", "password");
    // Note the error could manifest as an Interrupted error sometimes due to the JavaScript
    // execution being interrupted.
    assert.commandFailedWithCode(regularUserDB.runCommand(commandToRun), [
        ErrorCodes.Interrupted,
        ErrorCodes.MaxTimeMSExpired,
    ]);

    // Expect a user with 'bypassDefaultMaxTimeMS' to succeed.
    const bypassUserDB = getConn(dbName, "bypassUser", "password");
    assert.commandWorked(bypassUserDB.runCommand(commandToRun));

    // Expect a user with 'bypassDefaultMaxTimeMS', but that specified a maxTimeMS on the query, to
    // fail due to timeout.
    assert.commandFailedWithCode(bypassUserDB.runCommand({...commandToRun, maxTimeMS: 1}), [
        ErrorCodes.Interrupted,
        ErrorCodes.MaxTimeMSExpired,
    ]);

    // Expect root user to bypass the default.
    const rootUserDB = adminDB.getSiblingDB(dbName);
    assert.commandWorked(rootUserDB.runCommand(commandToRun));

    // Unsets the default MaxTimeMS to make queries not to time out in the
    // following code.
    setDefaultReadMaxTimeMS(adminDB, 0);
}

const keyFile = "jstests/libs/key1";
// Standard replica set test.
{
    const rst = new ReplSetTest({nodes: 1, keyFile: keyFile});
    rst.startSet();
    rst.initiate();

    const conn = rst.getPrimary();
    const getConn = (dbName, user, password) => {
        const newConn = new Mongo(conn.host);
        const adminDB = newConn.getDB("admin");
        assert.eq(1, adminDB.auth(user, password));
        return adminDB.getSiblingDB(dbName);
    };

    const commandToRun = setup(conn, getConn);
    runBypassTests(getConn, commandToRun);

    rst.stopSet();
}

// Sharded test.
{
    const st = new ShardingTest({
        mongos: 1,
        shards: {nodes: 1},
        config: {nodes: 1},
        keyFile: keyFile,
        mongosOptions: {
            setParameter: {"failpoint.skipClusterParameterRefresh": "{'mode':'alwaysOn'}"},
        },
    });

    const conn = st.s;
    const getConn = (dbName, user, password) => {
        const newConn = new Mongo(conn.host);
        const adminDB = newConn.getDB("admin");
        assert.eq(1, adminDB.auth(user, password));
        return adminDB.getSiblingDB(dbName);
    };

    const commandToRun = setup(conn, getConn);
    runBypassTests(getConn, commandToRun);

    st.stop();
}
