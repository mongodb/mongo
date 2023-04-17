/*
 * Tests internal client validation properties for txnRetryCounter.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
"use strict";

const kCollName = "testColl";

function verifyInternalSessionsForExternalClients(testDB, {expectFail}) {
    function runInternalSessionCommand(cmd) {
        if (expectFail) {
            const res =
                assert.commandFailedWithCode(testDB.runCommand(cmd), ErrorCodes.InvalidOptions);
            assert.eq(
                res.errmsg, "Internal sessions are only allowed for internal clients", tojson(res));
        } else {
            assert.commandWorked(testDB.runCommand(cmd));
            assert.commandWorked(testDB.runCommand({
                commitTransaction: 1,
                lsid: cmd.lsid,
                txnNumber: cmd.txnNumber,
                txnRetryCounter: cmd.txnRetryCounter,
                autocommit: false,
            }));
        }
    }

    // Test with both internal transaction formats.
    runInternalSessionCommand({
        find: kCollName,
        lsid: {id: UUID(), txnUUID: UUID()},
        txnNumber: NumberLong(1101),
        startTransaction: true,
        autocommit: false,
    });
    runInternalSessionCommand({
        find: kCollName,
        lsid: {id: UUID(), txnNumber: NumberLong(3), txnUUID: UUID()},
        txnNumber: NumberLong(1101),
        startTransaction: true,
        autocommit: false,
    });
}

function verifyTxnRetryCounterForExternalClients(testDB, {expectFail}) {
    const findCmd = {
        find: kCollName,
        lsid: {id: UUID()},
        txnNumber: NumberLong(1100),
        startTransaction: true,
        autocommit: false,
        txnRetryCounter: NumberInt(0),
    };

    if (expectFail) {
        const res =
            assert.commandFailedWithCode(testDB.runCommand(findCmd), ErrorCodes.InvalidOptions);
        assert.eq(res.errmsg, "txnRetryCounter is only allowed for internal clients", tojson(res));
    } else {
        assert.commandWorked(testDB.runCommand(findCmd));
        assert.commandWorked(testDB.runCommand({
            commitTransaction: 1,
            lsid: findCmd.lsid,
            txnNumber: findCmd.txnNumber,
            txnRetryCounter: findCmd.txnRetryCounter,
            autocommit: false,
        }));
    }
}

const keyFile = "jstests/libs/key1";
const st = new ShardingTest({shards: 1, config: 1, other: {keyFile}});

jsTestLog("Verify internal session and txnRetryCounter require internal privileges on mongod");

// Auth as a user with enough privileges to read from any collection, but not to identify as an
// internal client.
const shardDB = st.rs0.getPrimary().getDB("admin");
shardDB.createUser({user: "admin", pwd: "password", roles: jsTest.adminUserRoles});
assert(shardDB.auth("admin", "password"));

verifyTxnRetryCounterForExternalClients(shardDB, {expectFail: true});
verifyInternalSessionsForExternalClients(shardDB, {expectFail: true});
shardDB.logout();

jsTestLog("Verify internal session and txnRetryCounter require internal privileges on mongos");

// Auth as a user with enough privileges to read from any collection, but not to identify as an
// internal client.
const mongosDB = st.s.getDB("admin");
if (!TestData.configShard) {
    // In config shard mode, the user made on the shard above is also a cluster global user.
    mongosDB.createUser({user: "admin", pwd: "password", roles: jsTest.adminUserRoles});
}
assert(mongosDB.auth("admin", "password"));

verifyTxnRetryCounterForExternalClients(mongosDB, {expectFail: true});
verifyInternalSessionsForExternalClients(mongosDB, {expectFail: true});
mongosDB.logout();

jsTestLog("Verify internal session and txnRetryCounter work with internal privileges");

authutil.asCluster(st.s, keyFile, function() {
    verifyTxnRetryCounterForExternalClients(mongosDB, {expectFail: false});
    verifyInternalSessionsForExternalClients(mongosDB, {expectFail: false});
});

st.stop();
})();
