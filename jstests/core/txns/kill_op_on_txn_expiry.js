// Test that ongoing operations in a transaction are interrupted when the transaction expires.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: endSession, setParameter.
//   not_allowed_with_signed_security_token,
//   uses_transactions
// ]

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";

const dbName = "test";
const collName = "kill_op_on_txn_expiry";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB[collName];

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
assert.commandWorked(testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

const sessionOptions = {
    causalConsistency: false,
};
const session = db.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb[collName];

// Need the original 'transactionLifetimeLimitSeconds' value so that we can reset it back at the
// end of the test.
let res = assert.commandWorked(db.adminCommand({getParameter: 1, transactionLifetimeLimitSeconds: 1}));
const originalTransactionLifetimeLimitSeconds = res.transactionLifetimeLimitSeconds;

// Decrease transactionLifetimeLimitSeconds so it expires faster
jsTest.log(
    "Decrease transactionLifetimeLimitSeconds from " + originalTransactionLifetimeLimitSeconds + " to 30 seconds.",
);
assert.commandWorked(db.adminCommand({setParameter: 1, transactionLifetimeLimitSeconds: 30}));

res = assert.commandWorked(db.adminCommand({getParameter: 1, AbortExpiredTransactionsSessionCheckoutTimeout: 1}));
const originalAbortExpiredTransactionsSessionCheckoutTimeout = res.AbortExpiredTransactionsSessionCheckoutTimeout;

// Increase the AbortExpiredTransactionsSessionCheckoutTimeout to prevent it from conflicting with
// the abortExpiredTransactions job on slower machines. This ensures that the
// abortExpiredTransactions job is responsible for aborting the transaction, even if it takes
// longer, allowing the test to function as intended.
jsTest.log(
    "Increase AbortExpiredTransactionsSessionCheckoutTimeout from " +
        originalAbortExpiredTransactionsSessionCheckoutTimeout +
        " ms to 5 minutes.",
);
assert.commandWorked(db.adminCommand({setParameter: 1, AbortExpiredTransactionsSessionCheckoutTimeout: 5 * 60 * 1000}));

try {
    jsTestLog("Starting transaction");

    let txnNumber = 0;
    assert.commandWorked(
        testColl.runCommand({
            insert: collName,
            documents: [{_id: 0}],
            txnNumber: NumberLong(txnNumber),
            startTransaction: true,
            autocommit: false,
            lsid: session.getSessionId(),
        }),
    );

    jsTestLog("Enabling fail point to block batch inserts");
    let failPoint = configureFailPoint(testDB, "hangDuringBatchInsert", {shouldCheckForInterrupt: true});
    // Clear ramlog so checkLog can't find log messages from the previous times this test was run.
    assert.commandWorked(testDB.adminCommand({clearLog: "global"}));

    jsTestLog("Starting insert operation in parallel thread");
    let workerThread = new Thread(
        (sessionId, txnNumber, dbName, collName) => {
            // Deserialize the session ID from its string representation.
            sessionId = eval("(" + sessionId + ")");

            let coll = db.getSiblingDB(dbName).getCollection(collName);
            assert.commandFailedWithCode(
                coll.runCommand({
                    insert: collName,
                    documents: [{_id: 1}],
                    txnNumber: NumberLong(txnNumber),
                    autocommit: false,
                    lsid: sessionId,
                }),
                ErrorCodes.TransactionExceededLifetimeLimitSeconds,
            );
        },
        tojson(session.getSessionId()),
        txnNumber,
        dbName,
        collName,
    );
    workerThread.start();

    jsTestLog("Wait for insert to be blocked");
    failPoint.wait();

    jsTestLog("Wait for the transaction to expire");
    checkLog.contains(
        db.getMongo(),
        new RegExp(
            "Aborting transaction because it has been running for longer than 'transactionLifetimeLimitSeconds'.*\"txnNumber\":" +
                txnNumber,
        ),
    );

    jsTestLog("Disabling fail point to enable insert to proceed and detect that the session " + "has been killed");
    failPoint.off();

    workerThread.join();
    assert(!workerThread.hasFailed());
} finally {
    // Must ensure that the transactionLifetimeLimitSeconds and
    // AbortExpiredTransactionsSessionCheckoutTimeout are reset so that it does not impact other
    // tests in the suite.
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            transactionLifetimeLimitSeconds: originalTransactionLifetimeLimitSeconds,
        }),
    );
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            AbortExpiredTransactionsSessionCheckoutTimeout: originalAbortExpiredTransactionsSessionCheckoutTimeout,
        }),
    );
}

session.endSession();
