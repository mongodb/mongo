/*
 * Tests that a transaction that failed with a transient transaction error
 * can be retried on a replica set and that its txnRetryCounter is persisted correctly
 * on all nodes.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");

const kDbName = "testDb";
const kCollName = "testColl";
const kNs = kDbName + "." + kCollName;

const st = new ShardingTest({shards: 1, rs: {nodes: 3}});
const shard0Rst = st.rs0;
const shard0Primary = shard0Rst.getPrimary();

const mongosTestDB = st.s.getDB(kDbName);
const shard0TestDB = shard0Primary.getDB(kDbName);
assert.commandWorked(mongosTestDB.createCollection(kCollName));

const kConfigTxnNs = "config.transactions";
const kOplogNs = "local.oplog.rs";

function testCommitAfterRetry(db, lsid, txnNumber) {
    const txnRetryCounter0 = NumberInt(0);
    const txnRetryCounter1 = NumberInt(1);

    jsTest.log(
        "Verify that the client can retry a transaction that failed with a transient " +
        "transaction error by attaching a higher txnRetryCounter and commit the transaction");
    configureFailPoint(shard0Primary,
                       "failCommand",
                       {
                           failInternalCommands: true,
                           failCommands: ["insert"],
                           errorCode: ErrorCodes.LockBusy,
                           namespace: kNs
                       },
                       {times: 1});
    const insertCmdObj0 = {
        insert: kCollName,
        documents: [{x: 0}],
        lsid: lsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    };
    assert.commandFailedWithCode(
        db.runCommand(Object.assign({}, insertCmdObj0, {txnRetryCounter: txnRetryCounter0})),
        ErrorCodes.LockBusy);
    assert.commandWorked(
        db.runCommand(Object.assign({}, insertCmdObj0, {txnRetryCounter: txnRetryCounter1})));

    jsTest.log("Verify that the client must attach the last used txnRetryCounter in all commands " +
               "in the transaction");
    const insertCmdObj1 = {
        insert: kCollName,
        documents: [{x: 1}],
        lsid: lsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(1),
        autocommit: false,
    };
    const insertRes0 = assert.commandFailedWithCode(
        db.runCommand(Object.assign({}, insertCmdObj1, {txnRetryCounter: txnRetryCounter0})),
        ErrorCodes.TxnRetryCounterTooOld);
    assert.eq(txnRetryCounter1, insertRes0.txnRetryCounter, insertRes0);
    // txnRetryCounter defaults to 0.
    const insertRes1 = assert.commandFailedWithCode(db.runCommand(insertCmdObj1),
                                                    ErrorCodes.TxnRetryCounterTooOld);
    assert.eq(txnRetryCounter1, insertRes1.txnRetryCounter, insertRes1);
    assert.commandWorked(
        db.runCommand(Object.assign({}, insertCmdObj1, {txnRetryCounter: txnRetryCounter1})));

    jsTest.log("Verify that the client must attach the last used txnRetryCounter in the " +
               "commitTransaction command");
    const commitCmdObj = {
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: txnNumber,
        autocommit: false,
    };
    const commitRes = assert.commandFailedWithCode(
        db.adminCommand(Object.assign({}, commitCmdObj, {txnRetryCounter: txnRetryCounter0})),
        ErrorCodes.TxnRetryCounterTooOld);
    assert.eq(txnRetryCounter1, commitRes.txnRetryCounter, commitRes);

    assert.commandWorked(
        db.adminCommand(Object.assign({}, commitCmdObj, {txnRetryCounter: txnRetryCounter1})));
}

function testAbortAfterRetry(db, lsid, txnNumber) {
    const txnRetryCounter0 = NumberInt(0);
    const txnRetryCounter1 = NumberInt(1);

    jsTest.log("Verify that the client can retry a transaction that failed with a transient " +
               "transaction error by attaching a higher txnRetryCounter and abort the transaction");
    configureFailPoint(shard0Primary,
                       "failCommand",
                       {
                           failInternalCommands: true,
                           failCommands: ["insert"],
                           errorCode: ErrorCodes.LockBusy,
                           namespace: kNs
                       },
                       {times: 1});
    const insertCmdObj0 = {
        insert: kCollName,
        documents: [{x: 0}],
        lsid: lsid,
        txnNumber: txnNumber,
        startTransaction: true,
        autocommit: false,
    };
    assert.commandFailedWithCode(
        db.runCommand(Object.assign({}, insertCmdObj0, {txnRetryCounter: txnRetryCounter0})),
        ErrorCodes.LockBusy);
    assert.commandWorked(
        db.runCommand(Object.assign({}, insertCmdObj0, {txnRetryCounter: txnRetryCounter1})));

    jsTest.log("Verify that the client must attach the last used txnRetryCounter in the " +
               "abortTransaction command");
    const abortCmdObj = {
        abortTransaction: 1,
        lsid: lsid,
        txnNumber: txnNumber,
        autocommit: false,
    };
    const abortRes = assert.commandFailedWithCode(
        db.adminCommand(Object.assign({}, abortCmdObj, {txnRetryCounter: txnRetryCounter0})),
        ErrorCodes.TxnRetryCounterTooOld);
    assert.eq(txnRetryCounter1, abortRes.txnRetryCounter, abortRes);

    assert.commandWorked(
        db.adminCommand(Object.assign({}, abortCmdObj, {txnRetryCounter: txnRetryCounter1})));
}

function testPersistence(shardRst, lsid, txnNumber, txnDocFilter, oplogEntryFilter) {
    const txnRetryCounter0 = NumberInt(0);
    const txnRetryCounter1 = NumberInt(1);
    let db = shardRst.getPrimary().getDB(kDbName);

    const insertCmdObj = {
        insert: kCollName,
        documents: [{x: 0}],
        lsid: lsid,
        txnNumber: txnNumber,
        startTransaction: true,
        autocommit: false,
        txnRetryCounter: txnRetryCounter1
    };
    assert.commandWorked(db.runCommand(insertCmdObj));

    const commitCmdObj = {
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: txnNumber,
        autocommit: false,
    };
    assert.commandWorked(
        db.adminCommand(Object.assign({}, commitCmdObj, {txnRetryCounter: txnRetryCounter1})));

    jsTest.log("Verify that txnRetryCounter is persisted on all nodes");
    shardRst.awaitReplication();
    shardRst.nodes.forEach(node => {
        const txnDoc = node.getCollection(kConfigTxnNs).findOne(txnDocFilter);
        assert.neq(null, txnDoc);
        assert.eq(txnNumber, txnDoc.txnNum);
        assert.eq(txnRetryCounter1, txnDoc.txnRetryCounter);
        assert.eq("committed", txnDoc.state);
        const oplogEntry = node.getCollection(kOplogNs).findOne(oplogEntryFilter);
        assert.neq(null, oplogEntry);
        assert.eq(txnNumber, oplogEntry.txnNumber, tojson(oplogEntry));
        assert.eq(txnRetryCounter1, oplogEntry.txnRetryCounter, tojson(oplogEntry));
    });

    shardRst.stopSet(null /* signal */, true /*forRestart */);
    shardRst.startSet({restart: true});
    db = shardRst.getPrimary().getDB(kDbName);

    jsTest.log("Verify that the client must attach the last used txnRetryCounter in " +
               "commitTransaction command after restart/failover");
    const commitRes = assert.commandFailedWithCode(
        db.adminCommand(Object.assign({}, commitCmdObj, {txnRetryCounter: txnRetryCounter0})),
        ErrorCodes.TxnRetryCounterTooOld);
    assert.eq(txnRetryCounter1, commitRes.txnRetryCounter);
    assert.commandWorked(
        db.adminCommand(Object.assign({}, commitCmdObj, {txnRetryCounter: txnRetryCounter1})));
}

function testNoPersistenceOfDefaultTxnRetryCounter(
    shardRst, lsid, txnNumber, txnDocFilter, oplogEntryFilter) {
    let db = shardRst.getPrimary().getDB(kDbName);

    const insertCmdObj = {
        insert: kCollName,
        documents: [{x: 0}],
        lsid: lsid,
        txnNumber: txnNumber,
        txnRetryCounter: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    };
    assert.commandWorked(db.runCommand(insertCmdObj));

    const commitCmdObj = {
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: txnNumber,
        autocommit: false,
    };
    assert.commandWorked(db.adminCommand(Object.assign({}, commitCmdObj)));

    jsTest.log("Verify that txnRetryCounter is not persisted for any of the nodes if " +
               "txnRetryCounter is the default value");
    shardRst.awaitReplication();
    shardRst.nodes.forEach(node => {
        const txnDoc = node.getCollection(kConfigTxnNs).findOne(txnDocFilter);
        assert.neq(null, txnDoc);
        assert.eq(txnNumber, txnDoc.txnNum);
        assert.eq(null, txnDoc.txnRetryCounter);
        assert.eq("committed", txnDoc.state);
        const oplogEntry = node.getCollection(kOplogNs).findOne(oplogEntryFilter);
        assert.neq(null, oplogEntry);
        assert.eq(txnNumber, oplogEntry.txnNumber, tojson(oplogEntry));
        assert.eq(null, oplogEntry.txnRetryCounter, tojson(oplogEntry));
    });
}

(() => {
    jsTest.log("Test transactions in a replica set");
    const sessionUUID = UUID();
    const lsid0 = {id: sessionUUID};
    testCommitAfterRetry(shard0TestDB, lsid0, NumberLong(0));
    testAbortAfterRetry(shard0TestDB, lsid0, NumberLong(1));

    const lsid1 = {id: sessionUUID, txnNumber: NumberLong(2), txnUUID: UUID()};
    testCommitAfterRetry(shard0TestDB, lsid1, NumberLong(0));
    testAbortAfterRetry(shard0TestDB, lsid1, NumberLong(1));

    const lsid2 = {id: sessionUUID, txnUUID: UUID()};
    testCommitAfterRetry(shard0TestDB, lsid2, NumberLong(0));
    testAbortAfterRetry(shard0TestDB, lsid2, NumberLong(1));
})();

(() => {
    jsTest.log("Test that txnRetryCounter is persisted in the config.transactions doc");
    const sessionUUID = UUID();
    const lsid0 = {id: sessionUUID};
    const txnNumber0 = NumberLong(0);
    const txnDocFilter0 = {"_id.id": lsid0.id};
    const oplogEntryFilter0 = {"lsid.id": lsid0.id};
    testPersistence(shard0Rst, lsid0, txnNumber0, txnDocFilter0, oplogEntryFilter0);

    const lsid1 = {id: sessionUUID, txnNumber: NumberLong(1), txnUUID: UUID()};
    const txnNumber1 = NumberLong(0);
    const txnDocFilter1 = {
        "_id.id": lsid1.id,
        "_id.txnNumber": lsid1.txnNumber,
        "_id.txnUUID": lsid1.txnUUID
    };
    const oplogEntryFilter1 = {
        "lsid.id": lsid1.id,
        "lsid.txnNumber": lsid1.txnNumber,
        "lsid.txnUUID": lsid1.txnUUID
    };
    testPersistence(shard0Rst, lsid1, txnNumber1, txnDocFilter1, oplogEntryFilter1);

    const lsid2 = {id: sessionUUID, txnUUID: UUID()};
    const txnNumber2 = NumberLong(2);
    const txnDocFilter2 = {"_id.id": lsid2.id, "_id.txnUUID": lsid2.txnUUID};
    const oplogEntryFilter2 = {"lsid.id": lsid2.id, "lsid.txnUUID": lsid2.txnUUID};
    testPersistence(shard0Rst, lsid2, txnNumber2, txnDocFilter2, oplogEntryFilter2);
})();

(() => {
    jsTest.log("Test that txnRetryCounter is not persisted in the config.transactions doc if the " +
               "active txnNumber corresponds to a retryable write");
    const lsid = {id: UUID()};
    const txnNumber = NumberLong(7);
    const txnDocFilter = {"_id.id": lsid.id, txnNum: txnNumber};
    assert.commandWorked(mongosTestDB.runCommand(
        {insert: kCollName, documents: [{x: 0}], lsid: lsid, txnNumber: txnNumber}));
    shard0Rst.awaitReplication();
    shard0Rst.nodes.forEach(node => {
        const txnDoc = node.getCollection(kConfigTxnNs).findOne(txnDocFilter);
        assert.neq(null, txnDoc);
        assert(!txnDoc.hasOwnProperty("txnRetryCounter"));
    });
})();

(() => {
    jsTest.log("Test that txnRetryCounter is not persisted in the config.transactions doc if the " +
               "txnRetryCounter is the default value.");
    const sessionUUID = UUID();
    const lsid0 = {id: sessionUUID};
    const txnNumber0 = NumberLong(0);
    const txnDocFilter0 = {"_id.id": lsid0.id};
    const oplogEntryFilter0 = {"lsid.id": lsid0.id};
    testNoPersistenceOfDefaultTxnRetryCounter(
        shard0Rst, lsid0, txnNumber0, txnDocFilter0, oplogEntryFilter0);
})();

st.stop();
})();
