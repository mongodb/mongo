/*
 * Test that chunk migration can migrate a retryable internal transaction whose oplog entries have
 * been truncated.
 *
 * @tags: [uses_transactions, requires_persistence]
 */
(function() {
'use strict';

// This test involves writing directly to the config.transactions collection which is not allowed
// in a session.
TestData.disableImplicitSessions = true;

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

const dbName = 'testDb';
const collName = 'testColl';
const ns = dbName + '.' + collName;
const testDB = st.s.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.name);

assert.commandWorked(st.s.getCollection(ns).createIndex({x: 1}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

assert.commandWorked(st.s.getDB(dbName).runCommand({insert: collName, documents: [{x: 1}]}));

const parentLsid = {
    id: UUID()
};
const parentTxnNumber = NumberLong(35);

const originalChildLsid = {
    id: parentLsid.id,
    txnNumber: parentTxnNumber,
    txnUUID: UUID()
};
const childTxnNumber = NumberLong(1);

const updateCmdObj = {
    update: collName,
    updates: [{q: {x: 1}, u: {$set: {y: 1}}}],
    stmtId: NumberInt(0),
};
const res0 = assert.commandWorked(testDB.runCommand(Object.assign({}, updateCmdObj, {
    lsid: originalChildLsid,
    txnNumber: childTxnNumber,
    autocommit: false,
    startTransaction: true
})));
assert.eq(res0.nModified, 1, res0);
assert.commandWorked(st.s.adminCommand({
    commitTransaction: 1,
    lsid: originalChildLsid,
    txnNumber: childTxnNumber,
    autocommit: false,
}));

// Manually update the config.transactions document for the retryable internal transaction to point
// to an invalid op time.
const shard0ConfigTxnsColl = st.rs0.getPrimary().getCollection("config.transactions");
const res1 = assert.commandWorked(shard0ConfigTxnsColl.update(
    {"_id.txnUUID": originalChildLsid.txnUUID},
    {$set: {lastWriteOpTime: {ts: new Timestamp(100, 1), t: NumberLong(1)}}}));
assert.eq(res1.nModified, 1, res1);

assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));

assert.commandFailedWithCode(testDB.runCommand(Object.assign(
                                 {}, updateCmdObj, {lsid: parentLsid, txnNumber: parentTxnNumber})),
                             ErrorCodes.IncompleteTransactionHistory);

const retryChildLsid = {
    id: parentLsid.id,
    txnNumber: parentTxnNumber,
    txnUUID: UUID()
};
assert.commandFailedWithCode(testDB.runCommand(Object.assign({}, updateCmdObj, {
    lsid: retryChildLsid,
    txnNumber: childTxnNumber,
    autocommit: false,
    startTransaction: true
})),
                             ErrorCodes.IncompleteTransactionHistory);

st.stop();
})();
