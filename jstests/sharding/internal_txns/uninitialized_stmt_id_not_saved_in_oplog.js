/*
 * Confirm that retryable writes written with stmtId = -1 do not store stmtId information and that
 * this behavior is preserved through the restart of the primary shard & mongos.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence]
 */
(function() {
'use strict';

load("jstests/sharding/libs/sharded_transactions_helpers.js");

const st = new ShardingTest({shards: 1});
const shard0Primary = st.rs0.getPrimary();

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

// Insert initial data.
assert.commandWorked(st.s.getCollection(ns).insert([{x: 0}]));

jsTest.log(
    "Insert documents in a retryable write with uninitialized and initialized stmtIds. Expect DuplicateKey error upon retry of insert after restarting the mongos and primary shard of command with uninitialized stmtId. Expect command with initialized stmtId to return that it has previously been tried.");
const lsid = {
    id: UUID(),
    txnNumber: NumberLong(5),
    txnUUID: UUID()
};
const txnNumber = NumberLong(0);
const document = {
    _id: 0
};

const lsidWithStmtId = {
    id: UUID(),
    txnNumber: NumberLong(5),
    txnUUID: UUID()
};
const txnNumberWithStmtId = NumberLong(1);
const documentWithStmtId = {
    _id: 1
};
const initializedStmtId = NumberInt(1);

const retryableInsertCommandObjUninitializedStmtId = {
    insert: collName,
    documents: [document],
    lsid: lsid,
    txnNumber: txnNumber,
    startTransaction: true,
    autocommit: false,
    stmtId: NumberInt(-1)
};

const retryableInsertCommandObjInitializedStmtId = {
    insert: collName,
    documents: [documentWithStmtId],
    lsid: lsidWithStmtId,
    txnNumber: txnNumberWithStmtId,
    startTransaction: true,
    autocommit: false,
    stmtId: initializedStmtId
};

// Insert both documents, with and without initialized stmtIds.
assert.commandWorked(st.s.getDB(dbName).runCommand(retryableInsertCommandObjUninitializedStmtId));
assert.commandWorked(
    st.s.adminCommand({commitTransaction: 1, lsid: lsid, txnNumber: txnNumber, autocommit: false}));
assert.commandWorked(st.s.getDB(dbName).runCommand(retryableInsertCommandObjInitializedStmtId));
assert.commandWorked(st.s.adminCommand({
    commitTransaction: 1,
    lsid: lsidWithStmtId,
    txnNumber: txnNumberWithStmtId,
    autocommit: false
}));

// Confirm documents were inserted.
assert.eq(document, st.s.getCollection(ns).findOne(document));
assert.eq(documentWithStmtId, st.s.getCollection(ns).findOne(documentWithStmtId));

// The applyOps for an uninitialized stmtId should not have a stmtId property.
let oplog = getOplogEntriesForTxn(st.rs0, lsid, txnNumber);
assert(!oplog[0].o.applyOps[0].hasOwnProperty("stmtId"));

// The applyOps for an initialized stmtId should have a populated stmtId property.
oplog = getOplogEntriesForTxn(st.rs0, lsidWithStmtId, txnNumberWithStmtId);
assert.eq(initializedStmtId, oplog[0].o.applyOps[0].stmtId);

// Force refresh of primary & mongos.
st.rs0.restart(shard0Primary);
st.rs0.waitForPrimary();
st.restartMongos(0);

// Retry the insert command with stmtId = -1. Expect the insert to be unsuccessful.
let res = assert.commandFailedWithCode(
    st.s.getDB(dbName).runCommand(retryableInsertCommandObjUninitializedStmtId),
    ErrorCodes.DuplicateKey);

// retriedStmtIds field should not be set.
assert(!res.hasOwnProperty("retriedStmtIds"));

// Retry insert command with stmtId = 1. Insert should be successful with the retriedStmtIds field
// populated.
res =
    assert.commandWorked(st.s.getDB(dbName).runCommand(retryableInsertCommandObjInitializedStmtId));
assert.eq(initializedStmtId, res.retriedStmtIds[0]);

// Confirm that documents are in the collection.
assert.eq(document, st.s.getCollection(ns).findOne(document));
assert.eq(documentWithStmtId, st.s.getCollection(ns).findOne(documentWithStmtId));

st.stop();
})();
