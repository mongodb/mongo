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
 *   featureFlagSecurityToken,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function setDefaultReadMaxTimeMS(db, newValue) {
    assert.commandWorked(
        db.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: newValue}}}));

    // Currently, the mongos cluster parameter cache is not updated on setClusterParameter. An
    // explicit call to getClusterParameter will refresh the cache.
    assert.commandWorked(db.runCommand({getClusterParameter: "defaultMaxTimeMS"}));
}

function setup(conn, getConn, multiTenancy = false) {
    // Create a global admin user.
    {
        const adminDB = conn.getDB("admin");
        adminDB.createUser({user: 'admin', pwd: 'admin', roles: ['root']});
    }

    // Fetch a new connection, this might seem redundant, but is intended to make this work for the
    // multi-tenancy case.
    const adminDB = getConn('admin', 'admin', 'admin');

    // Prepare a regular user without the 'bypassDefaultMaxtimeMS' privilege.
    adminDB.createUser({user: 'regularUser', pwd: 'password', roles: ["readAnyDatabase"]});

    // Prepare a user with the 'bypassDefaultMaxtimeMS' privilege.
    adminDB.createRole({
        role: "bypassDefaultMaxtimeMSRole",
        privileges: [
            {resource: {cluster: true}, actions: ["bypassDefaultMaxTimeMS"]},
        ],
        roles: []
    });

    adminDB.createUser({
        user: 'bypassUser',
        pwd: 'password',
        roles: ["readAnyDatabase", "bypassDefaultMaxtimeMSRole"]
    });

    if (multiTenancy) {
        return {sleep: 1, millis: 300};
    } else {
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
                        body: function() {
                            sleep(1000);
                            return true;
                        },
                        args: [],
                        lang: "js"
                    }
                }
            }
        };

        return {
            aggregate: collName,
            pipeline: [slowStage],
            cursor: {},
        };
    }
}

function runBypassTests(getConn, commandToRun, dbName = jsTestName()) {
    const adminDB = getConn('admin', 'admin', 'admin');
    // Sets the default maxTimeMS for read operations with a small value.
    setDefaultReadMaxTimeMS(adminDB, 1);

    // Expect failure for the regular user.
    const regularUserDB = getConn(dbName, 'regularUser', 'password');
    // Note the error could manifest as an Interrupted error sometimes due to the JavaScript
    // execution being interrupted.
    assert.commandFailedWithCode(regularUserDB.runCommand(commandToRun),
                                 [ErrorCodes.Interrupted, ErrorCodes.MaxTimeMSExpired]);

    // Expect a user with 'bypassDefaultMaxTimeMS' to succeed.
    const bypassUserDB = getConn(dbName, 'bypassUser', 'password');
    assert.commandWorked(bypassUserDB.runCommand(commandToRun));

    // Expect a user with 'bypassDefaultMaxTimeMS', but that specified a maxTimeMS on the query, to
    // fail due to timeout.
    assert.commandFailedWithCode(bypassUserDB.runCommand({...commandToRun, maxTimeMS: 1}),
                                 [ErrorCodes.Interrupted, ErrorCodes.MaxTimeMSExpired]);

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
        mongosOptions:
            {setParameter: {'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"}},
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

// Multi-tenant test.
{
    const vtsKey = "secret";
    const rstWithTenants = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            setParameter: {
                multitenancySupport: true,
                testOnlyValidatedTenancyScopeKey: vtsKey,
            }
        },
        keyFile: keyFile
    });
    rstWithTenants.startSet();
    rstWithTenants.initiate();

    const conn = rstWithTenants.getPrimary();

    const tenantId1 = ObjectId();
    const unsignedToken1 = _createTenantToken({tenant: tenantId1});
    const getConnWithGlobalUser = (dbName, user, password) => {
        const newConn = new Mongo(conn.host);
        newConn._setSecurityToken(unsignedToken1);
        const newConnDB = newConn.getDB(dbName);
        assert.eq(1, newConnDB.auth(user, password));
        return newConnDB;
    };
    const getConn = (dbName, user, password) => {
        // setClusterParameter is only possible with a global user with useTenant.
        if (user == 'admin') {
            return getConnWithGlobalUser(dbName, user, password);
        }

        const newConn = new Mongo(conn.host);
        const securityToken =
            _createSecurityToken({user: user, db: 'admin', tenant: tenantId1}, vtsKey);
        newConn._setSecurityToken(securityToken);
        return newConn.getDB(dbName);
    };

    const commandToRun = setup(conn, getConnWithGlobalUser, true);
    runBypassTests(getConn, commandToRun, "admin");

    rstWithTenants.stopSet();
}
