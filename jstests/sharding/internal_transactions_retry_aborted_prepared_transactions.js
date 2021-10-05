/**
 * Test that mongos can retry an aborted transaction that successfully prepared on a subset of the
 * participant shards using a higher txnRetryCounter.
 *
 * @tags: [requires_fcv_51, featureFlagInternalTransactions, uses_transactions,
 * uses_multi_shard_transaction]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/sharding/libs/sharded_transactions_helpers.js");

const st = new ShardingTest({
    mongos: 2,
    shards: 2,
});
const shard1Primary = st.rs1.getPrimary();
enableCoordinateCommitReturnImmediatelyAfterPersistingDecision(st);

const kDbName = "testDb";
const kCollName = "testColl";
const kNs = kDbName + "." + kCollName;

assert.commandWorked(st.s0.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, st.shard0.shardName);
assert.commandWorked(st.s0.adminCommand({shardCollection: kNs, key: {x: 1}}));

// Make both shards have chunks for the collection so that two-phase commit is required.
assert.commandWorked(st.s0.adminCommand({split: kNs, middle: {x: 0}}));
assert.commandWorked(st.s0.adminCommand({moveChunk: kNs, find: {x: 0}, to: st.shard1.shardName}));

// Do an insert to force a refresh so the transaction doesn't fail due to StaleConfig.
assert.commandWorked(st.s0.getCollection(kNs).insert({x: 0}));

const lsid = {
    id: UUID()
};

const txnNumber0 = 0;
let txnRetryCounter0 = 1;
const insertCmdObj = {
    insert: kCollName,
    documents: [{x: -10}, {x: 10}],
    lsid: lsid,
    txnNumber: NumberLong(txnNumber0),
    startTransaction: true,
    autocommit: false,
};
const commitCmdObj = {
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(txnNumber0),
    autocommit: false
};

jsTest.log(
    "Force the transaction to successfully prepare on one of the participant shards and then abort");
configureFailPoint(shard1Primary,
                   "failCommand",
                   {
                       failInternalCommands: true,
                       failCommands: ["prepareTransaction"],
                       errorCode: ErrorCodes.NoSuchTransaction,
                   },
                   {times: 1});
const insertRes = assert.commandWorked(st.s0.getDB(kDbName).runCommand(
    Object.assign({}, insertCmdObj, {txnRetryCounter: NumberInt(txnRetryCounter0)})));
assert.commandFailedWithCode(st.s0.adminCommand(Object.assign(
                                 {}, commitCmdObj, {txnRetryCounter: NumberInt(txnRetryCounter0)})),
                             ErrorCodes.NoSuchTransaction);
// Verify that the abort decision can be recovered from both the original mongos and and the other
// mongos.
assert.commandFailedWithCode(st.s0.adminCommand(Object.assign(
                                 {}, commitCmdObj, {txnRetryCounter: NumberInt(txnRetryCounter0)})),
                             ErrorCodes.NoSuchTransaction);
assert.commandFailedWithCode(
    st.s1.adminCommand(Object.assign(
        {},
        commitCmdObj,
        {txnRetryCounter: NumberInt(txnRetryCounter0), recoveryToken: insertRes.recoveryToken})),
    ErrorCodes.NoSuchTransaction);
jsTest.log(
    "Verify after the transaction aborted, retrying commitTransaction with mismatching txnRetryCounter fails");
assert.commandFailedWithCode(st.s0.adminCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(txnNumber0),
    txnRetryCounter: NumberInt(txnRetryCounter0 - 1),
    autocommit: false
}),
                             ErrorCodes.TxnRetryCounterTooOld);
// Cannot recover commit decision without recoveryToken.
txnRetryCounter0++;
assert.commandFailedWithCode(st.s0.adminCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(txnNumber0),
    txnRetryCounter: NumberInt(txnRetryCounter0),
    autocommit: false,
}),
                             50940);
assert.commandFailedWithCode(st.s0.adminCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(txnNumber0),
    txnRetryCounter: NumberInt(txnRetryCounter0),
    autocommit: false,
    recoveryToken: insertRes.recoveryToken
}),
                             ErrorCodes.IllegalOperation);

jsTest.log(
    "Verify that the transaction can be retried and re-committed using a higher txnRetryCounter");
txnRetryCounter0++;
const retryInsertRes = assert.commandWorked(st.s0.getDB(kDbName).runCommand(
    Object.assign({}, insertCmdObj, {txnRetryCounter: NumberInt(txnRetryCounter0)})));
assert.commandWorked(st.s0.adminCommand(
    Object.assign({}, commitCmdObj, {txnRetryCounter: NumberInt(txnRetryCounter0)})));
// Verify that the commit decision can be recovered from both the original mongos and and the other
// mongos.
assert.commandWorked(st.s0.adminCommand(
    Object.assign({}, commitCmdObj, {txnRetryCounter: NumberInt(txnRetryCounter0)})));
assert.commandWorked(st.s1.adminCommand(Object.assign(
    {},
    commitCmdObj,
    {txnRetryCounter: NumberInt(txnRetryCounter0), recoveryToken: retryInsertRes.recoveryToken})));

jsTest.log(
    "Verify that after the retry the client can run a transaction with a higher txnNumber that requires two-phase commit");
const txnNumber1 = 1;
const txnRetryCounter1 = 1;
const updateRes = assert.commandWorked(st.s0.getDB(kDbName).runCommand({
    update: kCollName,
    updates: [{q: {x: -10}, u: {$set: {y: -10}}}, {q: {x: 10}, u: {$set: {y: 10}}}],
    lsid: lsid,
    txnNumber: NumberLong(txnNumber1),
    txnRetryCounter: NumberInt(txnRetryCounter1),
    startTransaction: true,
    autocommit: false,
}));
assert.commandWorked(st.s0.adminCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(txnNumber1),
    txnRetryCounter: NumberInt(txnRetryCounter1),
    autocommit: false
}));
jsTest.log(
    "Verify after the transaction committed, retrying commitTransaction with mismatching txnRetryCounter fails");
assert.commandFailedWithCode(st.s0.adminCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(txnNumber1),
    txnRetryCounter: NumberInt(txnRetryCounter1 - 1),
    autocommit: false
}),
                             ErrorCodes.TxnRetryCounterTooOld);
assert.commandFailedWithCode(st.s0.adminCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(txnNumber1),
    txnRetryCounter: NumberInt(txnRetryCounter1 + 1),
    autocommit: false,
}),
                             ErrorCodes.IllegalOperation);
assert.commandFailedWithCode(st.s0.adminCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(txnNumber1),
    txnRetryCounter: NumberInt(txnRetryCounter1 + 1),
    autocommit: false,
    recoveryToken: updateRes.recoveryToken
}),
                             ErrorCodes.IllegalOperation);

st.stop();
})();
