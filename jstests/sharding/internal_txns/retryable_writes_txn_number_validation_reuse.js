/*
 * Test that transaction participants validate the txnNumber in the lsids for internal transactions
 * for retryable writes against the txnNumber in the parent session and the txnNumber in the lsids
 * for internal transactions for other retryable writes. In particular, test that they:
 * - Throw an IncompleteTransactionHistory error upon seeing a txnNumber previously used by a
 *   transaction being used by a retryable internal transaction.
 * - Throw an error upon seeing a txnNumber previously used by a retryable internal transaction
 *   being used by another transaction, unless the retryable internal transaction has aborted
 *   without prepare.
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

const kTxnState = {
    kStarted: 1,
    kCommitted: 2,
    kAbortedWithoutPrepare: 3,
    kAbortedWithPrepare: 4
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

function testTxnNumberValidation(
    makeSessionOptsFunc, prevTxnStateName, testModeName, expectedError) {
    if ((prevTxnStateName == "kStarted" || prevTxnStateName == "kAbortedWithoutPrepare") &&
        (testModeName != "kNonRecovery")) {
        // Invalid combination. Only transactions that have been prepared or committed are expected
        // to survive failover and restart.
        return;
    }

    const {sessionOpts0, sessionOpts1} = makeSessionOptsFunc();
    const prevTxnState = kTxnState[prevTxnStateName];
    const testMode = kTestMode[testModeName];

    jsTest.log(`Test txnNumber validation upon running ${tojson(sessionOpts1)} after ${
        tojson(sessionOpts0)} reaches state ${prevTxnStateName} with test mode ${testModeName}`);

    assert.commandWorked(shard0TestDB.runCommand(makeInsertCmdObj([{x: 0}], sessionOpts0)));
    switch (prevTxnState) {
        case kTxnState.kCommitted:
            assert.commandWorked(shard0TestDB.adminCommand(
                makeCommitTransactionCmdObj(sessionOpts0.lsid, sessionOpts0.txnNumber)));
            break;
        case kTxnState.kAbortedWithoutPrepare:
            assert.commandWorked(shard0TestDB.adminCommand(
                makeAbortTransactionCmdObj(sessionOpts0.lsid, sessionOpts0.txnNumber)));
            break;
        case kTxnState.kAbortedWithPrepare:
            assert.commandWorked(shard0TestDB.adminCommand(
                makePrepareTransactionCmdObj(sessionOpts0.lsid, sessionOpts0.txnNumber)));
            assert.commandWorked(shard0TestDB.adminCommand(
                makeAbortTransactionCmdObj(sessionOpts0.lsid, sessionOpts0.txnNumber)));
            break;
        default:
            break;
    }

    setUpTestMode(testMode);

    const reuseRes = shard0TestDB.runCommand(makeInsertCmdObj([{x: 1}], sessionOpts1));
    if (expectedError) {
        assert.commandFailedWithCode(reuseRes, expectedError);
    } else {
        assert.commandWorked(reuseRes);
    }

    assert.commandWorked(mongosTestColl.remove({}));
}

// Test transaction number validation for internal transactions for writes with a txnNumber (i.e.
// retryable writes).

{
    let makeSessionOptsFunc = () => {
        const sessionUUID = UUID();
        // Transaction in the parent session.
        const sessionOpts0 = {
            lsid: {id: sessionUUID},
            txnNumber: NumberLong(5),
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
    const expectedError = ErrorCodes.IncompleteTransactionHistory;

    for (let testModeName in kTestMode) {
        for (let txnStateName in kTxnState) {
            testTxnNumberValidation(makeSessionOptsFunc, txnStateName, testModeName, expectedError);
        }
    }
}

{
    let makeSessionOptsFunc = () => {
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
            txnNumber: NumberLong(5),
            isTransaction: true
        };
        return {sessionOpts0, sessionOpts1};
    };
    const expectedError = 6202002;

    for (let testModeName in kTestMode) {
        for (let txnStateName in kTxnState) {
            testTxnNumberValidation(
                makeSessionOptsFunc,
                txnStateName,
                testModeName,
                txnStateName == "kAbortedWithoutPrepare" ? null : expectedError);
        }
    }
}

// Test that there is no "cross-session" transaction number validation for internal transactions for
// writes without a txnNumber (i.e. non-retryable writes).

{
    let makeSessionOptsFunc = () => {
        const sessionUUID = UUID();
        // Internal transaction for non-retryable writes in a child session.
        const sessionOpts0 = {
            lsid: {id: sessionUUID},
            txnNumber: NumberLong(5),
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
    const expectedError = null;

    testTxnNumberValidation(makeSessionOptsFunc, "kStarted", "kNonRecovery", expectedError);
}

{
    let makeSessionOptsFunc = () => {
        const sessionUUID = UUID();
        // Internal transaction for non-retryable writes in a child session.
        const sessionOpts0 = {
            lsid: {id: sessionUUID, txnUUID: UUID()},
            txnNumber: NumberLong(5),
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
    const expectedError = null;

    for (let txnStateName in kTxnState) {
        testTxnNumberValidation(makeSessionOptsFunc, txnStateName, "kNonRecovery", expectedError);
    }
}

{
    let makeSessionOptsFunc = () => {
        const sessionUUID = UUID();
        // Internal transaction for non-retryable writes in a child session.
        const sessionOpts0 = {
            lsid: {id: sessionUUID, txnUUID: UUID()},
            txnNumber: NumberLong(5),
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
    const expectedError = null;

    for (let txnStateName in kTxnState) {
        testTxnNumberValidation(makeSessionOptsFunc, txnStateName, "kNonRecovery", expectedError);
    }
}

{
    let makeSessionOptsFunc = () => {
        const sessionUUID = UUID();
        // Internal transaction for retryable writes in a child session.
        const sessionOpts0 = {
            lsid: {id: sessionUUID, txnNumber: NumberLong(5), txnUUID: UUID()},
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
    const expectedError = null;

    for (let txnStateName in kTxnState) {
        testTxnNumberValidation(makeSessionOptsFunc, txnStateName, "kNonRecovery", expectedError);
    }
}

st.stop();
})();
