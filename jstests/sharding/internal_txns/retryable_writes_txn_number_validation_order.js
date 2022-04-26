/*
 * Test that transaction participants validate the txnNumber in the lsids for internal transactions
 * for retryable writes against the txnNumber in the parent session and the txnNumber in the lsids
 * for internal transactions for other retryable writes. In particular, test that they throw a
 * TransactionTooOld error upon seeing a txnNumber lower than the highest seen one.
 *
 * Also test that transaction participants do not validate the txnNumber for internal transactions
 * for writes without a txnNumber (i.e. non-retryable writes) against the txnNumber in
 * the parent session and the txnNumber in the lsids for internal transactions for retryable writes.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence]
 */
(function() {
'use strict';

load('jstests/sharding/libs/sharded_transactions_helpers.js');

const kDbName = "testDb";
const kCollName = "testColl";

const st = new ShardingTest({shards: 1});
const shard0Primary = st.rs0.getPrimary();

const mongosTestDB = st.s.getDB(kDbName);
const mongosTestColl = mongosTestDB.getCollection(kCollName);
let shard0TestDB = shard0Primary.getDB(kDbName);

assert.commandWorked(mongosTestDB.createCollection(kCollName));

const kTestMode = {
    kNonRecovery: 1,
    kRestart: 2,
    kFailover: 3
};

function setUpTestMode(mode) {
    if (mode == kTestMode.kRestart) {
        st.rs0.stopSet(null /* signal */, true /*forRestart */);
        st.rs0.startSet({restart: true});
        shard0TestDB = st.rs0.getPrimary().getDB(kDbName);
    } else if (mode == kTestMode.kFailover) {
        const oldPrimary = st.rs0.getPrimary();
        assert.commandWorked(
            oldPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(oldPrimary.adminCommand({replSetFreeze: 0}));
        shard0TestDB = st.rs0.getPrimary().getDB(kDbName);
    }
}

function makeInsertCmdObj(docs, {lsid, txnNumber, isTransaction}) {
    const cmdObj = {
        insert: kCollName,
        documents: docs,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
    };
    if (isTransaction) {
        cmdObj.startTransaction = true;
        cmdObj.autocommit = false;
    }
    return cmdObj;
}

// Test transaction number validation for internal transactions for writes with a txnNumber (i.e.
// retryable writes).

function testTxnNumberValidationOnRetryingOldTxnNumber(makeOrderedSessionOptsFunc, testModeName) {
    const {sessionOpts0, sessionOpts1} = makeOrderedSessionOptsFunc();
    const testMode = kTestMode[testModeName];

    jsTest.log(`Testing txnNumber validation upon retrying ${tojson(sessionOpts0)} after running ${
        tojson(sessionOpts1)} with test mode ${testModeName}`);

    assert.commandWorked(shard0TestDB.runCommand(makeInsertCmdObj([{x: 0}], sessionOpts0)));
    if (sessionOpts0.isTransaction) {
        assert.commandWorked(shard0TestDB.adminCommand(
            makeCommitTransactionCmdObj(sessionOpts0.lsid, sessionOpts0.txnNumber)));
    }
    assert.commandWorked(shard0TestDB.runCommand(makeInsertCmdObj([{x: 1}], sessionOpts1)));
    if (sessionOpts1.isTransaction) {
        assert.commandWorked(shard0TestDB.adminCommand(
            makeCommitTransactionCmdObj(sessionOpts1.lsid, sessionOpts1.txnNumber)));
    }
    assert.eq(mongosTestColl.count(), 2);

    setUpTestMode(testMode);

    assert.commandFailedWithCode(shard0TestDB.runCommand(makeInsertCmdObj([{x: 0}], sessionOpts0)),
                                 ErrorCodes.TransactionTooOld);
    assert.eq(mongosTestColl.count(), 2);

    assert.commandWorked(mongosTestColl.remove({}));
}

function testTxnNumberValidationOnStartingOldTxnNumber(makeOrderedSessionOptsFunc, testModeName) {
    const orderedSessionsOpts = makeOrderedSessionOptsFunc();
    const sessionOpts0 = orderedSessionsOpts.sessionOpts1;
    const sessionOpts1 = orderedSessionsOpts.sessionOpts0;
    const testMode = kTestMode[testModeName];

    jsTest.log(`Testing txnNumber validation upon starting ${tojson(sessionOpts1)} after running ${
        tojson(sessionOpts0)} with test mode ${testModeName}`);

    assert.commandWorked(shard0TestDB.runCommand(makeInsertCmdObj([{x: 0}], sessionOpts0)));
    if (sessionOpts0.isTransaction && (testModeName != "kNonRecovery")) {
        // Only transactions that have been prepared or committed are expected to survive failover
        // and restart.
        assert.commandWorked(shard0TestDB.adminCommand(
            makeCommitTransactionCmdObj(sessionOpts0.lsid, sessionOpts0.txnNumber)));
    }

    setUpTestMode(testMode);

    assert.commandFailedWithCode(shard0TestDB.runCommand(makeInsertCmdObj([{x: 1}], sessionOpts1)),
                                 ErrorCodes.TransactionTooOld);

    if (sessionOpts0.isTransaction) {
        assert.commandWorked(shard0TestDB.adminCommand(
            makeCommitTransactionCmdObj(sessionOpts0.lsid, sessionOpts0.txnNumber)));
    }

    assert.eq(mongosTestColl.count(), 1);
    assert.neq(mongosTestColl.findOne({x: 0}), null);
    assert.commandWorked(mongosTestColl.remove({}));
}

function runTestsForInternalSessionForRetryableWrite(makeOrderedSessionOptsFunc) {
    for (let testModeName in kTestMode) {
        testTxnNumberValidationOnRetryingOldTxnNumber(makeOrderedSessionOptsFunc, testModeName);
        testTxnNumberValidationOnStartingOldTxnNumber(makeOrderedSessionOptsFunc, testModeName);
    }
}

{
    let makeOrderedSessionOptsFunc = () => {
        const sessionUUID = UUID();
        // Internal transaction for retryable writes in a child session.
        const sessionOpts0 = {
            lsid: {id: sessionUUID, txnNumber: NumberLong(5), txnUUID: UUID()},
            txnNumber: NumberLong(0),
            isTransaction: true
        };
        // Internal transaction for retryable writes in a child session.
        const sessionOpts1 = {
            lsid: {id: sessionUUID, txnNumber: NumberLong(6), txnUUID: UUID()},
            txnNumber: NumberLong(0),
            isTransaction: true
        };
        return {sessionOpts0, sessionOpts1};
    };

    runTestsForInternalSessionForRetryableWrite(makeOrderedSessionOptsFunc);
}

{
    let makeOrderedSessionOptsFunc = () => {
        const sessionUUID = UUID();
        // Internal transaction for retryable writes in a child session.
        const sessionOpts0 = {
            lsid: {id: sessionUUID, txnNumber: NumberLong(5), txnUUID: UUID()},
            txnNumber: NumberLong(0),
            isTransaction: true
        };
        // Retryable writes in the parent session.
        const sessionOpts1 = {
            lsid: {id: sessionUUID},
            txnNumber: NumberLong(6),
            isTransaction: false
        };
        return {sessionOpts0, sessionOpts1};
    };

    runTestsForInternalSessionForRetryableWrite(makeOrderedSessionOptsFunc);
}

{
    let makeOrderedSessionOptsFunc = () => {
        const sessionUUID = UUID();
        // Internal transaction for retryable writes in a child session.
        const sessionOpts0 = {
            lsid: {id: sessionUUID, txnNumber: NumberLong(5), txnUUID: UUID()},
            txnNumber: NumberLong(0),
            isTransaction: true
        };
        // Transaction in the parent session.
        const sessionOpts1 = {
            lsid: {id: sessionUUID},
            txnNumber: NumberLong(6),
            isTransaction: true
        };
        return {sessionOpts0, sessionOpts1};
    };

    runTestsForInternalSessionForRetryableWrite(makeOrderedSessionOptsFunc);
}

{
    let makeOrderedSessionOptsFunc = () => {
        const sessionUUID = UUID();
        // Retryable writes in the parent session.
        const sessionOpts0 = {
            lsid: {id: sessionUUID},
            txnNumber: NumberLong(5),
            isTransaction: false
        };
        // Internal transaction for retryable writes in a child session.
        const sessionOpts1 = {
            lsid: {id: sessionUUID, txnNumber: NumberLong(6), txnUUID: UUID()},
            txnNumber: NumberLong(0),
            isTransaction: true
        };
        return {sessionOpts0, sessionOpts1};
    };

    runTestsForInternalSessionForRetryableWrite(makeOrderedSessionOptsFunc);
}

{
    let makeOrderedSessionOptsFunc = () => {
        const sessionUUID = UUID();
        // Transaction in the parent session.
        const sessionOpts0 = {
            lsid: {id: sessionUUID},
            txnNumber: NumberLong(5),
            isTransaction: true
        };
        // Internal transaction for retryable writes in a child session.
        const sessionOpts1 = {
            lsid: {id: sessionUUID, txnNumber: NumberLong(6), txnUUID: UUID()},
            txnNumber: NumberLong(0),
            isTransaction: true
        };
        return {sessionOpts0, sessionOpts1};
    };

    runTestsForInternalSessionForRetryableWrite(makeOrderedSessionOptsFunc);
}

// Test that there is no "cross-session" transaction number validation for internal transactions for
// writes without a txnNumber (i.e. non-retryable writes).

function runTestForInternalSessionForNonRetryableWrite(makeSessionOptsFunc) {
    const {sessionOpts0, sessionOpts1} = makeSessionOptsFunc();

    jsTest.log(`Testing that there is no txnNumber validation upon starting ${
        tojson(sessionOpts1)} after running ${tojson(sessionOpts0)}`);

    assert.commandWorked(shard0TestDB.runCommand(makeInsertCmdObj([{x: 0}], sessionOpts0)));
    assert.commandWorked(shard0TestDB.runCommand(makeInsertCmdObj([{x: 1}], sessionOpts1)));

    if (sessionOpts0.isTransaction) {
        assert.commandWorked(shard0TestDB.adminCommand(
            makeCommitTransactionCmdObj(sessionOpts0.lsid, sessionOpts0.txnNumber)));
    }
    if (sessionOpts1.isTransaction) {
        assert.commandWorked(shard0TestDB.adminCommand(
            makeCommitTransactionCmdObj(sessionOpts1.lsid, sessionOpts1.txnNumber)));
    }

    assert.eq(mongosTestColl.count(), 2);
    assert.neq(mongosTestColl.findOne({x: 0}), null);
    assert.neq(mongosTestColl.findOne({x: 1}), null);
    assert.commandWorked(mongosTestColl.remove({}));
}

{
    let makeSessionOptsFunc = () => {
        const sessionUUID = UUID();
        // Internal transaction for non-retryable writes in a child session.
        const sessionOpts0 = {
            lsid: {id: sessionUUID},
            txnNumber: NumberLong(6),
            isTransaction: false
        };
        // Internal transaction for non-retryable writes in another child session.
        const sessionOpts1 = {
            lsid: {id: sessionUUID, txnUUID: UUID()},
            txnNumber: NumberLong(5),
            isTransaction: true
        };
        return {sessionOpts0, sessionOpts1};
    };

    runTestForInternalSessionForNonRetryableWrite(makeSessionOptsFunc);
}

{
    let makeSessionOptsFunc = () => {
        const sessionUUID = UUID();
        // Internal transaction for non-retryable writes in a child session.
        const sessionOpts0 = {
            lsid: {id: sessionUUID, txnUUID: UUID()},
            txnNumber: NumberLong(6),
            isTransaction: true
        };
        // Retryable writes in the parent session.
        const sessionOpts1 = {
            lsid: {id: sessionUUID},
            txnNumber: NumberLong(5),
            isTransaction: false
        };
        return {sessionOpts0, sessionOpts1};
    };

    runTestForInternalSessionForNonRetryableWrite(makeSessionOptsFunc);
}

{
    let makeSessionOptsFunc = () => {
        const sessionUUID = UUID();
        // Internal transaction for non-retryable writes in a child session.
        const sessionOpts0 = {
            lsid: {id: sessionUUID, txnUUID: UUID()},
            txnNumber: NumberLong(6),
            isTransaction: true
        };
        // Internal transaction for retryable writes in a child session.
        const sessionOpts1 = {
            lsid: {id: sessionUUID, txnNumber: NumberLong(5), txnUUID: UUID()},
            txnNumber: NumberLong(0),
            isTransaction: true
        };
        return {sessionOpts0, sessionOpts1};
    };

    runTestForInternalSessionForNonRetryableWrite(makeSessionOptsFunc);
}

{
    let makeSessionOptsFunc = () => {
        const sessionUUID = UUID();
        // Internal transaction for retryable writes in a child session.
        const sessionOpts0 = {
            lsid: {id: sessionUUID, txnNumber: NumberLong(6), txnUUID: UUID()},
            txnNumber: NumberLong(0),
            isTransaction: true
        };
        // Internal transaction for non-retryable writes in another child session.
        const sessionOpts1 = {
            lsid: {id: sessionUUID, txnUUID: UUID()},
            txnNumber: NumberLong(5),
            isTransaction: true
        };
        return {sessionOpts0, sessionOpts1};
    };

    runTestForInternalSessionForNonRetryableWrite(makeSessionOptsFunc);
}

st.stop();
})();
