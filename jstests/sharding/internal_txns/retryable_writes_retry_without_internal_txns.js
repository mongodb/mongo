/*
 * Test that retryable writes executed using or without using internal transactions execute exactly
 * once regardless of how they are retried.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence]
 */
import {
    withRetryOnTransientTxnErrorIncrementTxnNum
} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {TransactionsUtil} from "jstests/libs/transactions_util.js";
import {awaitRSClientHosts} from "jstests/replsets/rslib.js";
import {makeCommitTransactionCmdObj} from "jstests/sharding/libs/sharded_transactions_helpers.js";

const st = new ShardingTest({
    shards: 1,
    rs: {nodes: 2},
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true
});

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
        st.getAllNodes().forEach((conn) => {
            awaitRSClientHosts(conn, {host: newPrimary.host}, {ok: true, ismaster: true});
        });
    } else if (mode == kTestMode.kFailover) {
        const oldPrimary = st.rs0.getPrimary();
        assert.commandWorked(
            oldPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(oldPrimary.adminCommand({replSetFreeze: 0}));
        const newPrimary = st.rs0.getPrimary();
        st.getAllNodes().forEach((conn) => {
            awaitRSClientHosts(conn, {host: newPrimary.host}, {ok: true, ismaster: true});
        });
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

        const childLsid1 = {
            id: parentLsid.id,
            txnNumber: NumberLong(parentTxnNumber),
            txnUUID: UUID()
        };
        const childTxnNumber1 = NumberLong(0);
        jsTest.log(`Running an update inside a retryable internal transaction with lsid ${
            tojson(childLsid1)}`);
        withRetryOnTransientTxnErrorIncrementTxnNum(childTxnNumber1, (txnNum) => {
            assert.commandWorked(testDB.runCommand({
                update: kCollName,
                updates: [{q: {x: 1}, u: {$inc: {y: 1}}}],
                lsid: childLsid1,
                txnNumber: NumberLong(txnNum),
                startTransaction: true,
                autocommit: false,
                stmtId: stmtId1,
            }));
            assert.commandWorked(
                testDB.adminCommand(makeCommitTransactionCmdObj(childLsid1, txnNum)));
        });
        assert.eq(testColl.find({x: 1, y: 1}).itcount(), 1);

        const childLsid2 = {
            id: parentLsid.id,
            txnNumber: NumberLong(parentTxnNumber),
            txnUUID: UUID()
        };
        const childTxnNumber2 = NumberLong(0);
        jsTest.log(`Running an update inside a retryable internal transaction with lsid ${
            tojson(childLsid2)}`);
        withRetryOnTransientTxnErrorIncrementTxnNum(childTxnNumber2, (txnNum) => {
            assert.commandWorked(testDB.runCommand({
                update: kCollName,
                updates: [{q: {x: 2}, u: {$inc: {y: 1}}}],
                lsid: childLsid2,
                txnNumber: NumberLong(txnNum),
                startTransaction: true,
                autocommit: false,
                stmtId: stmtId2,
            }));
            assert.commandWorked(
                testDB.adminCommand(makeCommitTransactionCmdObj(childLsid2, txnNum)));
        });
        assert.eq(testColl.find({x: 2, y: 1}).itcount(), 1);

        setUpTestMode(testMode);

        jsTest.log(`Retrying both updates as retryable writes with lsid ${tojson(parentLsid)}`);

        // Retry the transaction if the RSM topology is not up to date and we receive a
        // TransientTransactionError. Remove after SERVER-60369 is completed.
        let retryRes;
        assert.soon(() => {
            try {
                retryRes = assert.commandWorked(testDB.runCommand({
                    update: kCollName,
                    updates: [{q: {x: 1}, u: {$inc: {y: 1}}}, {q: {x: 2}, u: {$inc: {y: 1}}}],
                    lsid: parentLsid,
                    txnNumber: NumberLong(parentTxnNumber),
                    stmtIds: [stmtId1, stmtId2]
                }));
                return true;
            } catch (e) {
                assert(ErrorCodes.isRetriableError(e.code) || isNetworkError(e));
                assert(TransactionsUtil.isTransientTransactionError(e));
            }
            return false;
        });
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

        const childLsid1 = {
            id: parentLsid.id,
            txnNumber: NumberLong(parentTxnNumber),
            txnUUID: UUID()
        };
        const childTxnNumber1 = NumberLong(0);
        jsTest.log(`Running an update in a retryable internal transaction with lsid ${
            tojson(childLsid1)}`);
        withRetryOnTransientTxnErrorIncrementTxnNum(childTxnNumber1, (txnNum) => {
            assert.commandWorked(testDB.runCommand({
                update: kCollName,
                updates: [{q: {x: 1}, u: {$inc: {y: 1}}}],
                lsid: childLsid1,
                txnNumber: NumberLong(txnNum),
                startTransaction: true,
                autocommit: false,
                stmtId: stmtId,
            }));
            assert.commandWorked(
                testDB.adminCommand(makeCommitTransactionCmdObj(childLsid1, txnNum)));
        });
        assert.eq(testColl.find({x: 1, y: 1}).itcount(), 1);

        setUpTestMode(testMode);

        const childLsid2 = {
            id: parentLsid.id,
            txnNumber: NumberLong(parentTxnNumber),
            txnUUID: UUID()
        };
        let childTxnNumber2 = NumberLong(0);
        jsTest.log(`Retrying the update in a retryable internal transaction with lsid ${
            tojson(childLsid2)}`);

        // Retry the transaction if the RSM topology is not up to date and we receive a
        // TransientTransactionError. Remove after SERVER-60369 is completed.
        let retryRes;
        assert.soon(() => {
            try {
                retryRes = assert.commandWorked(testDB.runCommand({
                    update: kCollName,
                    updates: [{q: {x: 1}, u: {$inc: {y: 1}}}],
                    lsid: childLsid2,
                    txnNumber: NumberLong(childTxnNumber2),
                    startTransaction: true,
                    autocommit: false,
                    stmtId: stmtId,
                }));
                return true;
            } catch (e) {
                assert(ErrorCodes.isRetriableError(e.code) || isNetworkError(e));
                assert(TransactionsUtil.isTransientTransactionError(e));
                childTxnNumber2++;
            }
            return false;
        });

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
            txnNumber: NumberLong(parentTxnNumber),
            stmtIds: [stmtId1, stmtId2]
        }));
        assert.eq(testColl.find({x: 1, y: 1}).itcount(), 1);
        assert.eq(testColl.find({x: 2, y: 1}).itcount(), 1);

        setUpTestMode(testMode);

        const childLsid1 = {
            id: parentLsid.id,
            txnNumber: NumberLong(parentTxnNumber),
            txnUUID: UUID()
        };
        let childTxnNumber1 = NumberLong(0);
        jsTest.log(`Retrying one of the updates in a retryable internal transaction with lsid ${
            tojson(childLsid1)}`);

        // Retry the transaction if the RSM topology is not up to date and we receive a
        // TransientTransactionError. Remove after SERVER-60369 is completed.
        let retryRes1;
        assert.soon(() => {
            try {
                retryRes1 = assert.commandWorked(testDB.runCommand({
                    update: kCollName,
                    updates: [{q: {x: 1}, u: {$inc: {y: 1}}}],
                    lsid: childLsid1,
                    txnNumber: NumberLong(childTxnNumber1),
                    startTransaction: true,
                    autocommit: false,
                    stmtId: stmtId1,
                }));
                return true;
            } catch (e) {
                assert(ErrorCodes.isRetriableError(e.code) || isNetworkError(e));
                assert(TransactionsUtil.isTransientTransactionError(e));
                childTxnNumber1++;
            }
            return false;
        });

        assert.commandWorked(
            testDB.adminCommand(makeCommitTransactionCmdObj(childLsid1, childTxnNumber1)));

        assert.eq(retryRes1.n, 1);
        assert.eq(testColl.find({x: 1, y: 1}).itcount(), 1);

        const childLsid2 = {
            id: parentLsid.id,
            txnNumber: NumberLong(parentTxnNumber),
            txnUUID: UUID()
        };
        const childTxnNumber2 = NumberLong(0);
        let retryRes2;
        jsTest.log(`Retrying one of the updates in a retryable internal transaction with lsid ${
            tojson(childLsid2)}`);
        withRetryOnTransientTxnErrorIncrementTxnNum(childTxnNumber2, (txnNum) => {
            retryRes2 = assert.commandWorked(testDB.runCommand({
                update: kCollName,
                updates: [{q: {x: 2}, u: {$inc: {y: 1}}}],
                lsid: childLsid2,
                txnNumber: NumberLong(txnNum),
                startTransaction: true,
                autocommit: false,
                stmtId: stmtId2,
            }));
            assert.commandWorked(
                testDB.adminCommand(makeCommitTransactionCmdObj(childLsid2, txnNum)));
        });

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
