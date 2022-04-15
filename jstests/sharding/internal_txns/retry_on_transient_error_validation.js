/*
 * Tests that txnRetryCounter is only supported in sharded clusters and in transactions.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");

const kDbName = "testDb";
const kCollName = "testColl";
const kNs = kDbName + "." + kCollName;

(() => {
    const rst = new ReplSetTest({nodes: 1});

    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const testDB = primary.getDB(kDbName);

    jsTest.log("Verify that txnRetryCounter is only supported in sharded clusters");
    const insertCmdObj = {
        insert: kCollName,
        documents: [{x: 0}],
        lsid: {id: UUID()},
        txnNumber: NumberLong(1),
        startTransaction: true,
        autocommit: false,
        txnRetryCounter: NumberInt(0)
    };
    assert.commandFailedWithCode(testDB.runCommand(insertCmdObj), ErrorCodes.InvalidOptions);
    rst.stopSet();
})();

(() => {
    const st = new ShardingTest({shards: 1});

    jsTest.log("Verify that txnRetryCounter is supported on shardvr");
    const shard0Primary = st.rs0.getPrimary();
    const insertCmdObjShardSvr = {
        insert: kCollName,
        documents: [{x: 0}],
        lsid: {id: UUID()},
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false,
        txnRetryCounter: NumberInt(0)
    };
    assert.commandWorked(shard0Primary.getDB(kDbName).runCommand(insertCmdObjShardSvr));
    assert.commandWorked(shard0Primary.adminCommand({
        abortTransaction: 1,
        lsid: insertCmdObjShardSvr.lsid,
        txnNumber: insertCmdObjShardSvr.txnNumber,
        autocommit: false,
        txnRetryCounter: NumberInt(0)
    }));

    jsTest.log("Verify that txnRetryCounter is supported on configsvr");
    const configRSPrimary = st.configRS.getPrimary();
    const deleteCmdObjConfigSvr = {
        delete: "chunks",
        deletes: [{q: {}, limit: 1}],
        lsid: {id: UUID()},
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false,
        txnRetryCounter: NumberInt(0)
    };
    assert.commandWorked(configRSPrimary.getDB("config").runCommand(deleteCmdObjConfigSvr));
    assert.commandWorked(configRSPrimary.adminCommand({
        abortTransaction: 1,
        lsid: deleteCmdObjConfigSvr.lsid,
        txnNumber: deleteCmdObjConfigSvr.txnNumber,
        autocommit: false,
    }));

    jsTest.log("Test that the client cannot specify txnRetryCounter in a retryable write command");
    const mongosTestDB = st.s.getDB(kDbName);
    const shard0TestDB = shard0Primary.getDB(kDbName);
    const insertCmdObj = {
        insert: kCollName,
        documents: [{x: 0}],
        lsid: {id: UUID()},
        txnNumber: NumberLong(0),
        txnRetryCounter: NumberInt(0)
    };
    assert.commandFailedWithCode(mongosTestDB.runCommand(insertCmdObj), ErrorCodes.InvalidOptions);

    assert.commandFailedWithCode(shard0TestDB.runCommand(insertCmdObj), ErrorCodes.InvalidOptions);

    st.stop();
})();
})();
