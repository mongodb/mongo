/**
 * Tests that operations killed due to 'defaultMaxTimeMS' and the user-specified 'maxTimeMS' options
 * will be recorded in the serverStatus metric accordingly.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   requires_auth,
 *   # Transactions aborted upon fcv upgrade or downgrade; cluster parameters use internal txns.
 *   uses_transactions,
 *   requires_fcv_80,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function setDefaultReadMaxTimeMS(db, newValue) {
    assert.commandWorked(
        db.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: newValue}}}));

    // Currently, the mongos cluster parameter cache is not updated on setClusterParameter. An
    // explicit call to getClusterParameter will refresh the cache.
    assert.commandWorked(db.runCommand({getClusterParameter: "defaultMaxTimeMS"}));
}

function runTests(conn, directConn) {
    const dbName = jsTestName();
    const adminDB = conn.getDB("admin");
    let connectionsToCheck = [adminDB];

    // Create the admin user, which is used to insert.
    adminDB.createUser({user: 'admin', pwd: 'admin', roles: ['root']});
    assert.eq(1, adminDB.auth("admin", "admin"));

    const testDB = adminDB.getSiblingDB(dbName);
    const collName = "test";
    const coll = testDB.getCollection(collName);

    for (let i = 0; i < 10; ++i) {
        assert.commandWorked(coll.insert({a: 1}));
    }

    // Prepare a regular user without the 'bypassDefaultMaxTimeMS' privilege.
    adminDB.createUser({user: 'regularUser', pwd: 'password', roles: ["readWriteAnyDatabase"]});
    // Prepare a user through the direct connection to the shard for getting serverStatus.
    if (directConn) {
        assert.eq(1, directConn.getDB("local").auth("__system", "foopdedoop"));
        directConn.getDB("admin").createUser(
            {user: "directUser", pwd: "password", roles: ["root"]});
        const directDB = new Mongo(directConn.host).getDB('admin');
        assert(directDB.auth('directUser', 'password'), "Auth failed");
        connectionsToCheck.push(directDB);
    }

    const regularUserConn = new Mongo(conn.host).getDB('admin');
    assert(regularUserConn.auth('regularUser', 'password'), "Auth failed");
    const regularUserDB = regularUserConn.getSiblingDB(dbName);

    // Sets the default maxTimeMS for read operations with a small value.
    setDefaultReadMaxTimeMS(adminDB, 1000);

    function assertCommandFailedWithMaxTimeMSExpired(cmd, metricField) {
        const beforeMetrics = connectionsToCheck.map((db) => {
            const serverStatus = assert.commandWorked(db.runCommand({serverStatus: 1}));
            return serverStatus.metrics.operation[[metricField]];
        });
        assert.commandFailedWithCode(regularUserDB.runCommand(cmd), ErrorCodes.MaxTimeMSExpired);
        connectionsToCheck.forEach((db, i) => {
            const serverStatus = assert.commandWorked(db.runCommand({serverStatus: 1}));
            assert.gt(serverStatus.metrics.operation[[metricField]], beforeMetrics[i]);
        });
    }

    // Times out due to the default value.
    assertCommandFailedWithMaxTimeMSExpired(
        {find: collName, filter: {$where: "sleep(1000); return true;"}},
        "killedDueToDefaultMaxTimeMSExpired");

    // Times out due to the request value.
    assertCommandFailedWithMaxTimeMSExpired(
        {find: collName, filter: {$where: "sleep(1000); return true;"}, maxTimeMS: 2000},
        "killedDueToMaxTimeMSExpired");

    // Times out due to the request value that is equal to the default value.
    assertCommandFailedWithMaxTimeMSExpired(
        {find: collName, filter: {$where: "sleep(1000); return true;"}, maxTimeMS: 1000},
        "killedDueToMaxTimeMSExpired");

    // Times out due to the default value.
    assertCommandFailedWithMaxTimeMSExpired(
        {count: collName, query: {$where: "sleep(1000); return true;"}},
        "killedDueToDefaultMaxTimeMSExpired");

    // Times out due to the default value.
    assertCommandFailedWithMaxTimeMSExpired(
        {distinct: collName, key: "a", query: {$where: "sleep(1000); return true;"}},
        "killedDueToDefaultMaxTimeMSExpired");

    adminDB.logout();
    regularUserDB.logout();
}

const rst = new ReplSetTest({
    nodes: 1,
    keyFile: "jstests/libs/key1",
});
rst.startSet();
rst.initiate();
runTests(rst.getPrimary());
rst.stopSet();

const st = new ShardingTest({
    mongos: 1,
    shards: {nodes: 1},
    config: {nodes: 1},
    keyFile: 'jstests/libs/key1',
    mongosOptions: {setParameter: {'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"}},
});
// Ensures the command times out on the shard but not the router.
const mongosFP = configureFailPoint(st.s.getDB("admin"), "maxTimeNeverTimeOut");
runTests(st.s, st.rs0.getPrimary());
mongosFP.off();
st.stop();
