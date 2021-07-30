/*
 * Tests basic support for internal sessions.
 *
 * @tags: [requires_fcv_51]
 */
(function() {
'use strict';

TestData.disableImplicitSessions = true;

const st = new ShardingTest({shards: 1});
const shard0Primary = st.rs0.getPrimary();

const kDbName = "testDb";
const kCollName = "testColl";
const testDB = st.s.getDB(kDbName);

const kConfigTxnNs = "config.transactions";

assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, st.shard0.shardName);

// Verify that parent and child sessions are tracked using different config.transactions documents.
const sessionUUID = UUID();

const lsid0 = {
    id: sessionUUID
};
assert.commandWorked(testDB.runCommand({
    insert: kCollName,
    documents: [{x: 0}],
    ordered: false,
    lsid: lsid0,
    txnNumber: NumberLong(0)
}));
assert.neq(null, shard0Primary.getCollection(kConfigTxnNs).findOne({"_id.id": sessionUUID}));

const lsid1 = {
    id: sessionUUID,
    txnNumber: NumberLong(35),
    stmtId: NumberInt(0)
};
assert.commandWorked(testDB.runCommand({
    insert: kCollName,
    documents: [{x: 1}],
    ordered: false,
    lsid: lsid1,
    txnNumber: NumberLong(0)
}));
assert.neq(null, shard0Primary.getCollection(kConfigTxnNs).findOne({
    "_id.id": sessionUUID,
    "_id.txnNumber": lsid1.txnNumber,
    "_id.stmtId": lsid1.stmtId
}));

const lsid2 = {
    id: sessionUUID,
    txnUUID: UUID()
};
assert.commandWorked(testDB.runCommand({
    insert: kCollName,
    documents: [{x: 2}],
    ordered: false,
    lsid: lsid2,
    txnNumber: NumberLong(35)
}));
assert.neq(null,
           shard0Primary.getCollection(kConfigTxnNs)
               .findOne({"_id.id": sessionUUID, "_id.txnUUID": lsid2.txnUUID}));

assert.eq(3, shard0Primary.getCollection(kConfigTxnNs).count({"_id.id": sessionUUID}));

st.stop();
})();
