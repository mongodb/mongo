/*
 * Test that retryable writes executed using or without using internal transactions execute exactly
 * once regardless of how they are retried.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence]
 */
(function() {
"use strict";

load("jstests/sharding/libs/sharded_transactions_helpers.js");

const st = new ShardingTest({shards: 1, rs: {nodes: 2}});

const kDbName = "testDb";
const kCollName = "testColl";
const testDB = st.s.getDB(kDbName);
const testColl = testDB.getCollection(kCollName);

const kTestMode = {
    kNonRecovery: 1,
    kRestart: 2,
    kFailover: 3
};

function setUpTestMode(mode) {
    if (mode == kTestMode.kRestart) {
        st.rs0.stopSet(null /* signal */, true /*forRestart */);
        st.rs0.startSet({restart: true});
        const newPrimary = st.rs0.getPrimary();
    } else if (mode == kTestMode.kFailover) {
        const oldPrimary = st.rs0.getPrimary();
        assert.commandWorked(
            oldPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(oldPrimary.adminCommand({replSetFreeze: 0}));
        const newPrimary = st.rs0.getPrimary();
    }
}

const sessionUUID = UUID();
const parentLsid = {
    id: sessionUUID
};
let currentParentTxnNumber = NumberLong(35);

{
    jsTest.log(
        "Test that retryable writes executed using internal transactions do not re-execute " +
        "when they are retried after they no longer need to be executed using internal transactions");

    let runTest = (testMode) => {
        const parentTxnNumber = NumberLong(currentParentTxnNumber++);
        const stmtId1 = NumberInt(1);
        const stmtId2 = NumberInt(2);

        assert.commandWorked(testColl.insert([{x: 1, y: 0}, {x: 2, y: 0}]));

        const childLsid1 = {id: parentLsid.id, txnNumber: parentTxnNumber, txnUUID: UUID()};
        const childTxnNumber1 = NumberLong(0);
        jsTest.log(`Running an update inside a retryable internal transaction with lsid ${
            tojson(childLsid1)}`);
        assert.commandWorked(testDB.runCommand({
            update: kCollName,
            updates: [{q: {x: 1}, u: {$inc: {y: 1}}}],
            lsid: childLsid1,
            txnNumber: childTxnNumber1,
            startTransaction: true,
            autocommit: false,
            stmtId: stmtId1,
        }));
        assert.commandWorked(
            testDB.adminCommand(makeCommitTransactionCmdObj(childLsid1, childTxnNumber1)));
        assert.eq(testColl.find({x: 1, y: 1}).itcount(), 1);

        const childLsid2 = {id: parentLsid.id, txnNumber: parentTxnNumber, txnUUID: UUID()};
        const childTxnNumber2 = NumberLong(0);
        jsTest.log(`Running an update inside a retryable internal transaction with lsid ${
            tojson(childLsid2)}`);
        assert.commandWorked(testDB.runCommand({
            update: kCollName,
            updates: [{q: {x: 2}, u: {$inc: {y: 1}}}],
            lsid: childLsid2,
            txnNumber: childTxnNumber2,
            startTransaction: true,
            autocommit: false,
            stmtId: stmtId2,
        }));
        assert.commandWorked(
            testDB.adminCommand(makeCommitTransactionCmdObj(childLsid2, childTxnNumber2)));
        assert.eq(testColl.find({x: 2, y: 1}).itcount(), 1);

        setUpTestMode(testMode);

        jsTest.log(`Retrying both updates as retryable writes with lsid ${tojson(parentLsid)}`);
        const retryRes = assert.commandWorked(testDB.runCommand({
            update: kCollName,
            updates: [{q: {x: 1}, u: {$inc: {y: 1}}}, {q: {x: 2}, u: {$inc: {y: 1}}}],
            lsid: parentLsid,
            txnNumber: parentTxnNumber,
            stmtIds: [stmtId1, stmtId2]
        }));
        assert.eq(retryRes.n, 2);
        assert.eq(testColl.find({x: 1, y: 1}).itcount(), 1);
        assert.eq(testColl.find({x: 2, y: 1}).itcount(), 1);

        assert.commandWorked(testColl.remove({}));
    };

    for (let modeName in kTestMode) {
        jsTest.log("Testing retry with testMode " + modeName);
        runTest(kTestMode[modeName]);
    }
}

{
    jsTest.log(
        "Test that retryable writes executed using internal transactions do not re-execute " +
        "when they are retried in internal transactions with lsids with different 'txnUUID'");

    let runTest = (testMode) => {
        const parentTxnNumber = NumberLong(currentParentTxnNumber++);
        const stmtId = NumberInt(1);

        assert.commandWorked(testColl.insert([{x: 1, y: 0}]));

        const childLsid1 = {id: parentLsid.id, txnNumber: parentTxnNumber, txnUUID: UUID()};
        const childTxnNumber1 = NumberLong(0);
        jsTest.log(`Running an update in a retryable internal transaction with lsid ${
            tojson(childLsid1)}`);
        assert.commandWorked(testDB.runCommand({
            update: kCollName,
            updates: [{q: {x: 1}, u: {$inc: {y: 1}}}],
            lsid: childLsid1,
            txnNumber: childTxnNumber1,
            startTransaction: true,
            autocommit: false,
            stmtId: stmtId,
        }));
        assert.commandWorked(
            testDB.adminCommand(makeCommitTransactionCmdObj(childLsid1, childTxnNumber1)));
        assert.eq(testColl.find({x: 1, y: 1}).itcount(), 1);

        setUpTestMode(testMode);

        const childLsid2 = {id: parentLsid.id, txnNumber: parentTxnNumber, txnUUID: UUID()};
        const childTxnNumber2 = NumberLong(0);
        jsTest.log(`Retrying the update in a retryable internal transaction with lsid ${
            tojson(childLsid2)}`);
        const retryRes = assert.commandWorked(testDB.runCommand({
            update: kCollName,
            updates: [{q: {x: 1}, u: {$inc: {y: 1}}}],
            lsid: childLsid2,
            txnNumber: childTxnNumber2,
            startTransaction: true,
            autocommit: false,
            stmtId: stmtId,
        }));
        assert.commandWorked(
            testDB.adminCommand(makeCommitTransactionCmdObj(childLsid2, childTxnNumber2)));
        assert.eq(retryRes.n, 1);
        assert.eq(testColl.find({x: 1, y: 1}).itcount(), 1);

        assert.commandWorked(testColl.remove({}));
    };

    for (let modeName in kTestMode) {
        jsTest.log("Testing retry with testMode " + modeName);
        runTest(kTestMode[modeName]);
    }
}

{
    jsTest.log("Test that retryable writes executed without using internal transactions do not " +
               "re-execute when they are retried while they need to be executed using internal " +
               "transactions");

    let runTest = (testMode) => {
        const parentTxnNumber = NumberLong(currentParentTxnNumber++);
        const stmtId1 = NumberInt(1);
        const stmtId2 = NumberInt(2);

        assert.commandWorked(testColl.insert([{x: 1, y: 0}, {x: 2, y: 0}]));

        jsTest.log(`Running updates as retryable writes with lsid ${tojson(parentLsid)}`);
        assert.commandWorked(testDB.runCommand({
            update: kCollName,
            updates: [{q: {x: 1}, u: {$inc: {y: 1}}}, {q: {x: 2}, u: {$inc: {y: 1}}}],
            lsid: parentLsid,
            txnNumber: parentTxnNumber,
            stmtIds: [stmtId1, stmtId2]
        }));
        assert.eq(testColl.find({x: 1, y: 1}).itcount(), 1);
        assert.eq(testColl.find({x: 2, y: 1}).itcount(), 1);

        setUpTestMode(testMode);

        const childLsid1 = {id: parentLsid.id, txnNumber: parentTxnNumber, txnUUID: UUID()};
        const childTxnNumber1 = NumberLong(0);
        jsTest.log(`Retrying one of the updates in a retryable internal transaction with lsid ${
            tojson(childLsid1)}`);
        const retryRes1 = assert.commandWorked(testDB.runCommand({
            update: kCollName,
            updates: [{q: {x: 1}, u: {$inc: {y: 1}}}],
            lsid: childLsid1,
            txnNumber: childTxnNumber1,
            startTransaction: true,
            autocommit: false,
            stmtId: stmtId1,
        }));
        assert.commandWorked(
            testDB.adminCommand(makeCommitTransactionCmdObj(childLsid1, childTxnNumber1)));
        assert.eq(retryRes1.n, 1);
        assert.eq(testColl.find({x: 1, y: 1}).itcount(), 1);

        const childLsid2 = {id: parentLsid.id, txnNumber: parentTxnNumber, txnUUID: UUID()};
        const childTxnNumber2 = NumberLong(0);
        jsTest.log(`Retrying one of the updates in a retryable internal transaction with lsid ${
            tojson(childLsid2)}`);
        const retryRes2 = assert.commandWorked(testDB.runCommand({
            update: kCollName,
            updates: [{q: {x: 2}, u: {$inc: {y: 1}}}],
            lsid: childLsid2,
            txnNumber: childTxnNumber2,
            startTransaction: true,
            autocommit: false,
            stmtId: stmtId2,
        }));
        assert.commandWorked(
            testDB.adminCommand(makeCommitTransactionCmdObj(childLsid2, childTxnNumber2)));
        assert.eq(retryRes2.n, 1);
        assert.eq(testColl.find({x: 2, y: 1}).itcount(), 1);

        assert.commandWorked(testColl.remove({}));
    };

    for (let modeName in kTestMode) {
        jsTest.log("Testing retry with testMode " + modeName);
        runTest(kTestMode[modeName]);
    }
}

st.stop();
})();
