/*
 * Tests running killSessions to kill internal sessions on both mongos and mongod.
 *
 * @tags: [requires_fcv_70, uses_transactions]
 */
(function() {
'use strict';

TestData.disableImplicitSessions = true;

const st = new ShardingTest({
    shards: 1,
    mongosOptions: {
        setParameter:
            {maxSessions: 1, 'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"}
    },
    // The config server uses a session for internal operations, so raise the limit by 1 for a
    // config shard.
    shardOptions: {setParameter: {maxSessions: TestData.configShard ? 2 : 1}}
});
const shard0Primary = st.rs0.getPrimary();

const kDbName = "testDb";
const kCollName = "testColl";
const testDB = st.s.getDB(kDbName);

const sessionUUID = UUID();

assert.commandWorked(testDB.runCommand({create: kCollName}));
assert.commandWorked(shard0Primary.adminCommand({refreshLogicalSessionCacheNow: 1}));

(() => {
    jsTest.log(
        "Test running killSessions on mongos using an internal transaction with lsid containing " +
        "txnNumber and txnUUID");
    const lsid = {id: sessionUUID, txnNumber: NumberLong(0), txnUUID: UUID()};
    const txnNumber = NumberLong(0);
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: lsid,
        txnNumber: txnNumber,
        startTransaction: true,
        autocommit: false
    }));

    assert.commandWorked(st.s.adminCommand({
        killSessions: [{id: sessionUUID}],
    }));

    // killSessions does not end/reap the session it targets. It only kills the operation running on
    // the session
    assert.commandFailedWithCode(
        testDB.adminCommand(
            {commitTransaction: 1, lsid: lsid, txnNumber: txnNumber, autocommit: false}),
        ErrorCodes.NoSuchTransaction);
})();

(() => {
    jsTest.log(
        "Test running killSessions on mongos using an internal transaction with lsid containing " +
        "txnUUID");
    const lsid = {id: sessionUUID, txnUUID: UUID()};
    const txnNumber = NumberLong(1);
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: lsid,
        txnNumber: txnNumber,
        startTransaction: true,
        autocommit: false
    }));

    assert.commandWorked(st.s.adminCommand({
        killSessions: [{id: sessionUUID}],
    }));

    // killSessions does not end/reap the session it targets. It only kills the operation running on
    // the session
    assert.commandFailedWithCode(
        testDB.adminCommand(
            {commitTransaction: 1, lsid: lsid, txnNumber: txnNumber, autocommit: false}),
        ErrorCodes.NoSuchTransaction);
})();

(() => {
    jsTest.log("Test running killSessions on mongos using an internal transaction with child lsid");
    const lsid = {id: sessionUUID, txnUUID: UUID()};
    const txnNumber = NumberLong(1);
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: lsid,
        txnNumber: txnNumber,
        startTransaction: true,
        autocommit: false
    }));

    assert.commandFailedWithCode(st.s.adminCommand({
        killSessions: [lsid],
    }),
                                 ErrorCodes.InvalidOptions);
})();

(() => {
    jsTest.log(
        "Test running killSessions on mongod using an internal transaction with lsid containing " +
        "txnNumber and txnUUID");
    const lsid = {id: sessionUUID, txnNumber: NumberLong(1), txnUUID: UUID()};
    const txnNumber = NumberLong(2);
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: lsid,
        txnNumber: txnNumber,
        startTransaction: true,
        autocommit: false
    }));

    assert.commandWorked(shard0Primary.adminCommand({
        killSessions: [{id: sessionUUID}],
    }));

    // killSessions does not end/reap the session it targets. It only kills the operation running on
    // the session
    assert.commandFailedWithCode(
        testDB.adminCommand(
            {commitTransaction: 1, lsid: lsid, txnNumber: txnNumber, autocommit: false}),
        ErrorCodes.NoSuchTransaction);
})();

(() => {
    jsTest.log(
        "Test running killSessions on mongod using an internal transaction with lsid containing " +
        "txnUUID");
    const lsid = {id: sessionUUID, txnUUID: UUID()};
    const txnNumber = NumberLong(3);
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: lsid,
        txnNumber: txnNumber,
        startTransaction: true,
        autocommit: false
    }));

    assert.commandWorked(shard0Primary.adminCommand({
        killSessions: [{id: sessionUUID}],
    }));

    // killSessions does not end/reap the session it targets. It only kills the operation running on
    // the session
    assert.commandFailedWithCode(
        testDB.adminCommand(
            {commitTransaction: 1, lsid: lsid, txnNumber: txnNumber, autocommit: false}),
        ErrorCodes.NoSuchTransaction);
})();

(() => {
    jsTest.log("Test running killSessions on mongod using an internal transaction with child lsid");
    const lsid = {id: sessionUUID, txnUUID: UUID()};
    const txnNumber = NumberLong(3);
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: lsid,
        txnNumber: txnNumber,
        startTransaction: true,
        autocommit: false
    }));

    assert.commandFailedWithCode(shard0Primary.adminCommand({
        killSessions: [lsid],
    }),
                                 ErrorCodes.InvalidOptions);
})();

st.stop();
})();
