/**
 * Tests that running the endSessions command for a parent session causes the config.transactions
 * and config.image_collection entries for its child sessions to get reaped.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */

(function() {
"use strict";

// This test makes assertions about the number of sessions, which are not compatible with
// implicit sessions.
TestData.disableImplicitSessions = true;

const st = new ShardingTest({
    shards: 1,
    shardOptions: {
        setParameter: {
            TransactionRecordMinimumLifetimeMinutes: 0,
            storeFindAndModifyImagesInSideCollection: true,
        }
    }
});

const shard0Rst = st.rs0;
const shard0Primary = shard0Rst.getPrimary();

const kDbName = "testDb";
const kCollName = "testColl";

const kConfigSessionsNs = "config.system.sessions";
const kConfigTxnsNs = "config.transactions";
const kImageCollNs = "config.image_collection";

let sessionsCollOnPrimary = shard0Primary.getCollection(kConfigSessionsNs);
let transactionsCollOnPrimary = shard0Primary.getCollection(kConfigTxnsNs);
let imageCollOnPrimary = shard0Primary.getCollection(kImageCollNs);
let testDB = shard0Primary.getDB(kDbName);

const sessionUUID = UUID();
const parentLsid = {
    id: sessionUUID
};

const kInternalTxnNumber = NumberLong(0);

let numTransactionsCollEntries = 0;
let numImageCollEntries = 0;

assert.commandWorked(
    testDB.runCommand({insert: kCollName, documents: [{_id: 0}], lsid: parentLsid}));

const childLsid0 = {
    id: sessionUUID,
    txnUUID: UUID()
};
assert.commandWorked(testDB.runCommand({
    update: kCollName,
    updates: [{q: {_id: 0}, u: {$set: {a: 0}}}],
    lsid: childLsid0,
    txnNumber: kInternalTxnNumber,
    startTransaction: true,
    autocommit: false
}));
assert.commandWorked(testDB.adminCommand(
    {commitTransaction: 1, lsid: childLsid0, txnNumber: kInternalTxnNumber, autocommit: false}));
numTransactionsCollEntries++;

assert.eq(numTransactionsCollEntries, transactionsCollOnPrimary.find().itcount());

const parentTxnNumber1 = NumberLong(1);

assert.commandWorked(testDB.runCommand({
    update: kCollName,
    updates: [{q: {_id: 0}, u: {$set: {b: 0}}}],
    lsid: parentLsid,
    txnNumber: parentTxnNumber1,
    stmtId: NumberInt(0)
}));
numTransactionsCollEntries++;

const childLsid1 = {
    id: sessionUUID,
    txnNumber: parentTxnNumber1,
    txnUUID: UUID()
};
assert.commandWorked(testDB.runCommand({
    findAndModify: kCollName,
    query: {_id: 0},
    update: {$set: {c: 0}},
    lsid: childLsid1,
    txnNumber: kInternalTxnNumber,
    stmtId: NumberInt(1),
    startTransaction: true,
    autocommit: false
}));
assert.commandWorked(testDB.adminCommand(
    {commitTransaction: 1, lsid: childLsid1, txnNumber: kInternalTxnNumber, autocommit: false}));
numTransactionsCollEntries++;
numImageCollEntries++;

assert.eq(numTransactionsCollEntries, transactionsCollOnPrimary.find().itcount());
assert.eq(numImageCollEntries, imageCollOnPrimary.find().itcount());

assert.commandWorked(shard0Primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
assert.eq(1, sessionsCollOnPrimary.find({"_id.id": sessionUUID}).itcount());

// Run endSessions for the parent session so it gets reaped when reapLogicalSessionCacheNow is run.
assert.commandWorked(shard0Primary.adminCommand({endSessions: [parentLsid]}));
assert.commandWorked(shard0Primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
assert.eq(0, sessionsCollOnPrimary.find({"_id.id": sessionUUID}).itcount());

assert.commandWorked(shard0Primary.adminCommand({reapLogicalSessionCacheNow: 1}));
jsTest.log(
    "Verify that the config.transactions entries and config.image_collection got reaped " +
    "since the config.system.sessions entry for the parent session had already been deleted");
assert.eq(0,
          transactionsCollOnPrimary.find().itcount(),
          tojson(transactionsCollOnPrimary.find().toArray()));
assert.eq(0, imageCollOnPrimary.find().itcount());

st.stop();
})();
